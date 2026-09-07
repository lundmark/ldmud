/* Deferred blueprint update request ownership and reporting.
 * This stage performs no compilation or migration. Terminal failure state is
 * reserved in the admission allocation, so backend completion never allocates.
 */
#include "driver.h"
#ifdef USE_BLUEPRINT_UPDATE
#include <assert.h>
#include "program_update.h"
#include "array.h"
#include "backend.h"
#include "exec.h"
#include "gcollect.h"
#include "interpret.h"
#include "main.h"
#include "mapping.h"
#include "mstrings.h"
#include "object.h"
#include "simulate.h"
#include "simul_efun.h"
#include "stdstrings.h"
#include "svalue.h"
#include "swap.h"
#include "xalloc.h"
#include "i-current_object.h"

#define BLUEPRINT_UPDATE_REPORT_TTL 300
#define BLUEPRINT_UPDATE_MAX_REPORTS 256

enum update_root
{
    UPDATE_ORIGIN, UPDATE_SOURCE, UPDATE_TARGETS, UPDATE_NUM_ROOTS
};
typedef struct program_update_request_s
{
    error_handler_t handler;
    struct program_update_request_s *next;
    struct program_update_request_s *owner_next;
    struct program_update_request_s *work_next;
    object_t *owner;              /* Non-owning; destruction unlinks it. */
    program_t *source_program;    /* Exact generation, independently pinned. */
    svalue_t roots[UPDATE_NUM_ROOTS];
    p_int id;
    time_t completed_at;
    Bool source_from_path;
    Bool explicit_selection;
    Bool terminal;
    Bool busy;                   /* Admission or current backend evaluation. */
    Bool canceled;
    Bool validation_failed;
} program_update_request_t;

/* Admission, detached batch, and terminal requests all stay on this root. */
static program_update_request_t *requests;
static program_update_request_t *pending;
static program_update_request_t *batch;
static program_update_request_t *active;
static p_int last_id;

static void
unlink_work(program_update_request_t **list, program_update_request_t *request)
{
    while (*list && *list != request)
        list = &(*list)->work_next;
    if (*list)
        *list = request->work_next;
}

static void
unlink_owner(program_update_request_t *request)
{
    program_update_request_t **link;
    if (!request->owner)
        return;
    link = &request->owner->program_updates;
    while (*link && *link != request)
        link = &(*link)->owner_next;
    if (*link)
        *link = request->owner_next;
    request->owner = NULL;
    request->owner_next = NULL;
}

static void
release_inputs(program_update_request_t *request)
{
    int i;
    for (i = UPDATE_SOURCE; i < UPDATE_NUM_ROOTS; i++)
    {
        free_svalue(&request->roots[i]);
        put_number(&request->roots[i], 0);
    }
    if (request->source_program)
    {
        free_prog(request->source_program, MY_TRUE);
        request->source_program = NULL;
    }
}

static void
free_request(program_update_request_t *request)
{
    program_update_request_t **link = &requests;
    unlink_owner(request);
    unlink_work(&pending, request);
    unlink_work(&batch, request);
    while (*link != request)
        link = &(*link)->next;
    *link = request->next;
    release_inputs(request);
    free_svalue(&request->roots[UPDATE_ORIGIN]);
    xfree(request);
}

static void
admission_cleanup(error_handler_t *handler)
{
    program_update_request_t *request = (program_update_request_t *)handler;
    if (!request->id)
        free_request(request);
}

static void
expire_reports(void)
{
    program_update_request_t *request, *next, *oldest;
    size_t count = 0;
    for (request = requests; request; request = next)
    {
        next = request->next;
        if (!request->terminal)
            continue;
        if (current_time - request->completed_at >= BLUEPRINT_UPDATE_REPORT_TTL)
            free_request(request);
        else
            count++;
    }
    while (count > BLUEPRINT_UPDATE_MAX_REPORTS)
    {
        oldest = NULL;
        for (request = requests; request; request = request->next)
            if (request->terminal
             && (!oldest || request->completed_at < oldest->completed_at
              || (request->completed_at == oldest->completed_at
               && request->id < oldest->id)))
                oldest = request;
        free_request(oldest);
        count--;
    }
}

static Bool
same_family(object_t *object, string_t *origin)
{
    return object->load_name && mstreq(object->load_name, origin);
}

static void
validate_object(object_t *object, string_t *origin, Bool source)
{
    const char *name, *program_name;
    size_t length;
    replace_ob_t *replacement;
    if (!object || object->flags & (O_DESTRUCTED | O_REPLACED | O_SHADOW)
     || object == master_ob || object == simul_efun_object)
        errorf("update_blueprint(): unsupported object.\n");
    /* Backup names may include a leading slash or .c suffix. Resolve only
     * an existing identity; admission must never load a sefun implicitly.
     */
    if (simul_efun_vector)
    {
        size_t i;

        for (i = 1; i < VEC_SIZE(simul_efun_vector); i++)
            if (simul_efun_vector->item[i].type == T_STRING
             && find_object(simul_efun_vector->item[i].u.str) == object)
                errorf("update_blueprint(): backup simul-efun object.\n");
    }
    if (source && (object->flags & O_CLONE))
        errorf("update_blueprint(): source must be a blueprint.\n");
    if (!same_family(object, origin))
        errorf("update_blueprint(): unrelated target.\n");
    if (object->flags & O_SWAPPED)
        if (load_ob_from_swap(object) < 0)
            errorf("update_blueprint(): cannot unswap object.\n");
    name = get_txt(origin);
    if (*name == '/')
        name++;
    length = strlen(name);
    program_name = get_txt(object->prog->name);
    if (strlen(program_name) != length + 2
     || strncmp(name, program_name, length)
     || strcmp(program_name + length, ".c")
     || (source && (object->prog->blueprint != object
                 || strcmp(get_txt(object->name), name))))
        errorf("update_blueprint(): virtual or replaced object.\n");
    for (replacement = obj_list_replace; replacement; replacement = replacement->next)
        if (same_family(replacement->ob, origin))
            errorf("update_blueprint(): pending replace_program conflict.\n");
}

static void
validate_targets(program_update_request_t *request, Bool admission)
{
    string_t *origin = request->roots[UPDATE_ORIGIN].u.str;
    size_t count = 0, scanned = 0, slots = 0, retained;
    object_t *object;
    vector_t *targets = request->explicit_selection
                        ? request->roots[UPDATE_TARGETS].u.vec : NULL;
    retained = sizeof(*request) + request->source_program->total_size;
    if (targets)
        retained += sizeof(*targets) + VEC_SIZE(targets) * sizeof(svalue_t);
    if (retained > BLUEPRINT_UPDATE_MAX_BYTES)
        errorf("update_blueprint(): retained storage limit exceeded.\n");
    for (object = targets ? NULL : obj_list; targets || object;
         object = targets ? NULL : object->next_all)
    {
        if (targets)
        {
            if (scanned == VEC_SIZE(targets))
                break;
            if (targets->item[scanned].type != T_OBJECT)
            {
                if (admission)
                    errorf("update_blueprint(): target was destructed during authorization.\n");
                scanned++;
                continue;
            }
            object = targets->item[scanned].u.ob;
        }
        if (++scanned > BLUEPRINT_UPDATE_MAX_SCAN_OBJECTS)
            errorf("update_blueprint(): object scan limit exceeded.\n");
        if (object->flags & O_DESTRUCTED)
        {
            if (admission)
                errorf("update_blueprint(): target was destructed during authorization.\n");
            continue;
        }
        if (!targets && (!(object->flags & O_CLONE) || !same_family(object, origin)))
            continue;
        validate_object(object, origin, MY_FALSE);
        if (++count > BLUEPRINT_UPDATE_MAX_TARGETS)
            errorf("update_blueprint(): target limit exceeded.\n");
        slots += object->prog->num_variables;
        if (slots > BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS
         || slots > (BLUEPRINT_UPDATE_MAX_BYTES - retained) / sizeof(svalue_t))
            errorf("update_blueprint(): variable storage limit exceeded.\n");
    }
}

static void
validate_capacity(program_update_request_t *self)
{
    program_update_request_t *request;
    size_t inflight = 0, owned = 0;
    for (request = requests; request; request = request->next)
    {
        if (request->terminal || request == self)
            continue;
        inflight++;
        if (request->owner == self->owner)
            owned++;
        if (request->roots[UPDATE_ORIGIN].type == T_STRING
         && mstreq(request->roots[UPDATE_ORIGIN].u.str,
                   self->roots[UPDATE_ORIGIN].u.str))
            errorf("update_blueprint(): family already has a pending request.\n");
    }
    if (inflight >= BLUEPRINT_UPDATE_MAX_INFLIGHT || owned >= BLUEPRINT_UPDATE_MAX_OWNER)
        errorf("update_blueprint(): pending request limit exceeded.\n");
}

svalue_t *
v_update_blueprint(svalue_t *sp, int num_arg)
{
    svalue_t *arg = sp - num_arg + 1;
    program_update_request_t *request, **tail;
    object_t *source;
    vector_t *selection;
    size_t i, j, unique = 0;
    inter_sp = sp;
    expire_reports();
    if (!get_current_object() || get_current_object()->flags & O_DESTRUCTED)
        errorf("update_blueprint(): caller has been destructed.\n");
    if (last_id == PINT_MAX)
        errorf("update_blueprint(): request identifiers exhausted.\n");
    if (num_arg == 2 && arg[1].type == T_NUMBER && arg[1].u.number)
        errorf("update_blueprint(): selection must be zero or an object array.\n");
    source = arg->type == T_OBJECT ? arg->u.ob : find_object(arg->u.str);
    if (!source)
        errorf("update_blueprint(): source blueprint is not loaded.\n");
    validate_object(source, source->load_name, MY_TRUE);
    request = xalloc(sizeof(*request));
    if (!request)
        outofmem(sizeof(*request), "blueprint update request");
    *request = (program_update_request_t){0};
    for (i = 0; i < UPDATE_NUM_ROOTS; i++)
        put_number(&request->roots[i], 0);
    request->busy = MY_TRUE;
    request->owner = get_current_object();
    request->owner_next = request->owner->program_updates;
    request->owner->program_updates = request;
    request->next = requests;
    requests = request;
    push_error_handler(admission_cleanup, &request->handler);
    put_ref_string(&request->roots[UPDATE_ORIGIN], source->load_name);
    put_ref_object(&request->roots[UPDATE_SOURCE], source, "blueprint update");
    request->source_program = source->prog;
    reference_prog(source->prog, "blueprint update");
    request->source_from_path = arg->type == T_STRING;
    request->explicit_selection = num_arg == 2 && arg[1].type == T_POINTER;
    if (request->source_program->total_size > BLUEPRINT_UPDATE_MAX_BYTES)
        errorf("update_blueprint(): retained program limit exceeded.\n");
    if (request->explicit_selection)
    {
        selection = arg[1].u.vec;
        if (VEC_SIZE(selection) > BLUEPRINT_UPDATE_MAX_TARGETS)
            errorf("update_blueprint(): explicit target limit exceeded.\n");
        for (i = 0; i < VEC_SIZE(selection); i++)
        {
            if (selection->item[i].type != T_OBJECT)
                errorf("update_blueprint(): target must be an object.\n");
            validate_object(selection->item[i].u.ob, source->load_name, MY_FALSE);
            for (j = 0; j < i; j++)
                if (selection->item[j].u.ob == selection->item[i].u.ob)
                    break;
            if (j == i)
                unique++;
        }
        put_array(&request->roots[UPDATE_TARGETS], allocate_array(unique));
        unique = 0;
        for (i = 0; i < VEC_SIZE(selection); i++)
        {
            for (j = 0; j < i; j++)
                if (selection->item[j].u.ob == selection->item[i].u.ob)
                    break;
            if (j == i)
                assign_svalue_no_free(&request->roots[UPDATE_TARGETS].u.vec->item[unique++],
                                     &selection->item[i]);
        }
    }
    validate_capacity(request);
    validate_targets(request, MY_TRUE);
    if (!privilege_violation2(STR_UPDATE_BLUEPRINT, arg,
                             num_arg == 2 ? &arg[1] : &const0, inter_sp))
        errorf("update_blueprint(): privilege denied.\n");
    if (request->canceled || !request->owner
     || request->roots[UPDATE_SOURCE].type != T_OBJECT)
        errorf("update_blueprint(): owner or source was destructed.\n");
    validate_object(source, request->roots[UPDATE_ORIGIN].u.str, MY_TRUE);
    if (source->prog != request->source_program)
        errorf("update_blueprint(): source program changed during authorization.\n");
    validate_capacity(request);
    validate_targets(request, MY_TRUE);
    if (last_id == PINT_MAX)
        errorf("update_blueprint(): request identifiers exhausted.\n");
    request->id = ++last_id;
    request->busy = MY_FALSE;
    tail = &pending;
    while (*tail)
        tail = &(*tail)->work_next;
    *tail = request;
    pop_stack(); /* Published requests survive their admission cleanup. */
    sp = pop_n_elems(num_arg, sp);
    push_number(sp, request->id);
    return sp;
}

/* The key and report stay rooted on the normal interpreter error stack. */
static svalue_t *
report_field(mapping_t *report, const char *name)
{
    svalue_t *result;
    push_c_string(inter_sp, name);
    result = get_map_lvalue(report, inter_sp);
    pop_stack();
    return result;
}

svalue_t *
f_update_blueprint_result(svalue_t *sp)
{
    static const char *numeric_fields[] =
    {
        "candidate_generation", "matched", "updated", "already_current",
        "destroyed", "blueprint_updated"
    };
    program_update_request_t *request;
    mapping_t *report, *error;
    svalue_t *field;
    size_t i;
    inter_sp = sp;
    expire_reports();
    for (request = requests; request; request = request->next)
        if (request->id && request->id == sp->u.number && !request->canceled)
            break;
    if (!request)
        errorf("update_blueprint_result(): unknown or expired request id.\n");
    if (get_current_object() != request->owner && get_current_object() != master_ob)
        errorf("update_blueprint_result(): request belongs to another object.\n");
    report = allocate_mapping(15, 1);
    if (!report)
        outofmem(15, "blueprint update report");
    put_mapping(sp, report);
    put_number(report_field(report, "id"), request->id);
    put_ref_string(report_field(report, "origin"), request->roots[UPDATE_ORIGIN].u.str);
    put_c_string(report_field(report, "selection"), request->explicit_selection ? "explicit" : "all");
    put_c_string(report_field(report, "status"), request->terminal ? "failed" : "pending");
    for (i = 0; i < sizeof(numeric_fields) / sizeof(numeric_fields[0]); i++)
        put_number(report_field(report, numeric_fields[i]), 0);
    put_number(report_field(report, "completed_at"), request->completed_at);
    field = report_field(report, "variable_changes");
    put_array(field, allocate_array(0));
    field = report_field(report, "errors");
    put_array(field, allocate_array(request->terminal ? 1 : 0));
    if (request->terminal)
    {
        error = allocate_mapping(2, 1);
        if (!error)
            outofmem(2, "blueprint update failure report");
        put_mapping(&field->u.vec->item[0], error);
        put_c_string(report_field(error, "code"), request->validation_failed
                     ? "VALIDATION_FAILED" : "IMPLEMENTATION_INCOMPLETE");
        put_c_string(report_field(error, "message"), request->validation_failed
                     ? "Blueprint update validation failed in the backend."
                     : "Blueprint migration is not implemented.");
    }
    return sp;
}

void
program_update_detach(void)
{
    assert(!batch);
    batch = pending;
    pending = NULL;
}

void
program_update_process(void)
{
    struct error_recovery_info recovery;
    expire_reports();
    recovery.rt.last = rt_context;
    recovery.rt.type = ERROR_RECOVERY_BACKEND;
    rt_context = &recovery.rt;
    while (batch)
    {
        active = batch;
        batch = active->work_next;
        active->work_next = NULL;
        active->busy = MY_TRUE;
        if (setjmp(recovery.con.text))
        {
            active->validation_failed = MY_TRUE;
            mark_end_evaluation();
            clear_state();
        }
        else if (!active->canceled)
        {
            mark_start_evaluation();
            validate_targets(active, MY_FALSE);
            /* Future compiler and migration hooks belong in this boundary. */
            mark_end_evaluation();
        }
        active->busy = MY_FALSE;
        if (active->canceled)
            free_request(active);
        else
        {
            release_inputs(active);
            active->terminal = MY_TRUE;
            active->completed_at = current_time;
        }
        active = NULL;
    }
    rt_context = recovery.rt.last;
    expire_reports();
}

void
program_update_owner_destructed(object_t *owner)
{
    while (owner->program_updates)
    {
        program_update_request_t *request = owner->program_updates;
        unlink_owner(request);
        if (request->busy)
            request->canceled = MY_TRUE;
        else
            free_request(request);
    }
}

void
program_update_shutdown(void)
{
    while (requests)
        free_request(requests);
}

#ifdef GC_SUPPORT
void
program_update_clear_refs(void)
{
    program_update_request_t *request;
    for (request = requests; request; request = request->next)
    {
        clear_memory_reference(request);
        clear_ref_in_vector(request->roots, UPDATE_NUM_ROOTS);
        if (request->source_program)
            clear_program_ref(request->source_program, MY_TRUE);
    }
}

void
program_update_count_refs(void)
{
    program_update_request_t *request;
    for (request = requests; request; request = request->next)
    {
        note_malloced_block_ref(request);
        count_ref_in_vector(request->roots, UPDATE_NUM_ROOTS);
        if (request->source_program)
            mark_program_ref(request->source_program);
    }
}
#endif /* GC_SUPPORT */

#ifdef DEBUG
void
program_update_count_extra_refs(void)
{
    program_update_request_t *request;
    for (request = requests; request; request = request->next)
    {
        count_extra_ref_in_vector(request->roots, UPDATE_NUM_ROOTS);
        if (request->source_program)
        {
            request->source_program->extra_ref++;
            count_extra_ref_in_prog(request->source_program);
        }
    }
}
#endif /* DEBUG */
#endif /* USE_BLUEPRINT_UPDATE */
