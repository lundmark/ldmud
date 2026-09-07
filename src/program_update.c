/* Deferred blueprint update request ownership and reporting.
 * This stage performs no compilation or migration. Terminal failure state is
 * reserved in the admission allocation. Schema diagnostics may allocate within
 * the backend recovery boundary, with partial summaries rooted in the request.
 */
#include "driver.h"
#ifdef USE_BLUEPRINT_UPDATE
#include <assert.h>
#include "program_update.h"
#include "program_schema.h"
#include "efuns.h"
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
    UPDATE_ORIGIN, UPDATE_SCHEMAS, UPDATE_SOURCE, UPDATE_TARGETS, UPDATE_NUM_ROOTS
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
    p_int candidate_generation;
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
unlink_work (program_update_request_t **list, program_update_request_t *request)

/* Remove <request> from the work chain headed by <list>, if present.
 * Neither request ownership nor its references change. The caller must keep
 * <request> rooted until any subsequent release is complete.
 */

{
    while (*list && *list != request)
        list = &(*list)->work_next;
    if (*list)
        *list = request->work_next;
} /* unlink_work() */

static void
unlink_owner (program_update_request_t *request)

/* Remove <request> from its owner's non-owning reverse chain and clear
 * both owner links. An already unlinked request is unchanged. No object
 * reference is released and no mudlib code is called.
 */

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
} /* unlink_owner() */

static void
release_inputs (program_update_request_t *request)

/* Release and zero the source and fixed target references of <request>,
 * then release its independently pinned program. The origin and reserved
 * terminal record survive. Repeated calls are harmless after completion.
 *
 * These native releases must not reenter LPC or garbage collection while
 * the record is being dismantled. TODO: Future compiler cleanup or native
 * finalizer hooks require a rooted retirement state and pointers cleared
 * before potentially reentrant releases, rather than this current contract.
 */

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
} /* release_inputs() */

static void
free_request (program_update_request_t *request)

/* Unlink the globally rooted <request> from all work and owner chains,
 * release its remaining references, and free the record. The caller must
 * ensure no stack handler or active evaluation will use it afterwards.
 *
 * This teardown is currently synchronous and non-reentrant. Once removed
 * from requests, the record is no longer a GC root. Future cleanup hooks
 * must provide a separate rooted retirement phase before changing this.
 */

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
    free_svalue(&request->roots[UPDATE_SCHEMAS]);
    xfree(request);
} /* free_request() */

static void
admission_cleanup (error_handler_t *handler)

/* Clean the request whose first member is <handler> when the interpreter
 * pops its admission error handler. An unpublished request (id zero) is
 * freed; successful admission leaves its globally rooted record intact.
 */

{
    program_update_request_t *request = (program_update_request_t *)handler;
    if (!request->id)
        free_request(request);
} /* admission_cleanup() */

static void
expire_reports (void)

/* Drop terminal reports older than the retention interval, then evict
 * the oldest remaining terminal reports until the cache fits its bound.
 * Pending, admitted, and currently executing requests remain rooted.
 * Only non-reentrant native teardown is allowed from this routine.
 */

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
} /* expire_reports() */

static Bool
same_family (object_t *object, string_t *origin)

/* Return true if <object>'s canonical load name equals <origin>.
 * Both arguments are borrowed references; no lookup or loading occurs.
 */

{
    return object->load_name && mstreq(object->load_name, origin);
} /* same_family() */

static void
validate_object (object_t *object, string_t *origin, Bool source)

/* Require <object> to be a supported, live member of canonical family
 * <origin>. With <source> true it must be the actual ordinary blueprint;
 * otherwise a same-family clone is also accepted. Reject special roles,
 * virtual/replaced objects, and pending replace_program conflicts.
 *
 * May unswap an existing object but never loads a missing blueprint.
 * Return normally on success; raise an LPC error on invalidity or failure.
 */

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
} /* validate_object() */

static void
validate_source (program_update_request_t *request)

/* Require <request>'s captured source to remain live and supported with
 * its exact pinned program. Admission and execution share this check; a
 * new blueprint at the same pathname cannot replace the captured identity.
 * Return normally on success, otherwise raise an LPC error.
 */

{
    object_t *source;

    if (request->roots[UPDATE_SOURCE].type != T_OBJECT)
        errorf("update_blueprint(): captured source was destructed.\n");
    source = request->roots[UPDATE_SOURCE].u.ob;
    validate_object(source, request->roots[UPDATE_ORIGIN].u.str, MY_TRUE);
    if (source->prog != request->source_program)
        errorf("update_blueprint(): captured source program changed.\n");
} /* validate_source() */

static void
validate_targets (program_update_request_t *request, Bool admission)

/* Validate <request>'s copied explicit selection or scan all currently
 * loaded matching clones. Enforce target, scan, variable-slot, and retained
 * storage bounds. With <admission> true, a destroyed explicit target is an
 * error; after admission it is skipped without extending the fixed set.
 *
 * The source program must still be pinned and non-NULL. No selection or
 * object program is changed. Invalid input or exceeded limits raise an
 * LPC error; otherwise return normally.
 */

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
} /* validate_targets() */

static void
describe_schemas (program_update_request_t *request)

/* Build immutable summaries for every represented old program. The source
 * and all live programs remain pinned through the object roots/list while
 * this native pass executes. No LPC callbacks run. Store partial results in
 * the global request root before any fallible work, including comparison.
 */
{
    vector_t *targets = request->explicit_selection
                        ? request->roots[UPDATE_TARGETS].u.vec : NULL;
    svalue_t *root = &request->roots[UPDATE_SCHEMAS];
    size_t scanned = 0, count = 0, work = 0;
    size_t remaining = BLUEPRINT_UPDATE_MAX_BYTES;
    size_t capacity = targets ? VEC_SIZE(targets) : 0;
    object_t *object;
    vector_t *trimmed;

    if (request->source_from_path)
        return;
    request->candidate_generation = request->source_program->schema_generation;
    if (!targets)
    {
        for (object = obj_list; object; object = object->next_all)
        {
            if (++scanned > BLUEPRINT_UPDATE_MAX_SCAN_OBJECTS)
                errorf("update_blueprint(): schema scan limit exceeded.\n");
            if (!(object->flags & O_DESTRUCTED) && (object->flags & O_CLONE)
             && same_family(object, request->roots[UPDATE_ORIGIN].u.str)
             && object->prog != request->source_program)
                capacity++;
        }
        scanned = 0;
    }
    put_array(root, allocate_array(capacity));
    for (object = targets ? NULL : obj_list; targets || object;
         object = targets ? NULL : object->next_all)
    {
        size_t previous;
        if (targets)
        {
            if (scanned == VEC_SIZE(targets))
                break;
            if (targets->item[scanned].type != T_OBJECT)
            {
                scanned++;
                continue;
            }
            object = targets->item[scanned].u.ob;
        }
        if (++scanned > BLUEPRINT_UPDATE_MAX_SCAN_OBJECTS)
            errorf("update_blueprint(): schema scan limit exceeded.\n");
        if (object->flags & O_DESTRUCTED)
            continue;
        if (!targets && (!(object->flags & O_CLONE)
                     || !same_family(object, request->roots[UPDATE_ORIGIN].u.str)))
            continue;
        if (object->prog == request->source_program)
            continue;
        for (previous = 0; previous < count; previous++)
        {
            svalue_t *generation;
            if (++work > BLUEPRINT_UPDATE_MAX_SCAN_OBJECTS)
                errorf("update_blueprint(): schema generation scan limit exceeded.\n");
            /* The temporary key is stack-rooted during lookup. */
            push_c_string(inter_sp, "old_generation");
            generation = get_map_value(root->u.vec->item[previous].u.map, inter_sp);
            pop_stack();
            if (generation->u.number == object->prog->schema_generation)
                break;
        }
        if (previous != count)
            continue;
        if (count == BLUEPRINT_UPDATE_MAX_TARGETS)
            errorf("update_blueprint(): schema generation limit exceeded.\n");
        program_schema_compare(object->prog, request->source_program,
                               &root->u.vec->item[count++], &remaining);
        if (!remaining)
            break;
    }
    trimmed = slice_array(root->u.vec, 0, count - 1);
    free_svalue(root);
    put_array(root, trimmed);
} /* describe_schemas() */

static void
validate_capacity (program_update_request_t *self)

/* Check whether the rooted, unpublished request <self> can be admitted.
 * Ignore terminal reports and <self> while counting driver-wide and owner
 * requests; reject any other request reserving the same canonical family.
 * Return normally when all limits hold, or raise an LPC error.
 */

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
} /* validate_capacity() */

svalue_t *
v_update_blueprint (svalue_t *sp, int num_arg)

/* LPC efun: int update_blueprint(string|object source,
 *                                void|int|object* targets)
 *
 * Queue a deferred request using an already loaded ordinary blueprint.
 * String sources resolve an existing canonical identity; object sources
 * must be actual blueprints. Omitted targets or integer zero select all
 * matching clones at execution. An array selects a copied, deduplicated
 * fixed set; an empty array selects no clones.
 *
 * Check master privilege and revalidate mutable facts before assigning a
 * positive driver-lifetime ID. Invalid admission raises without consuming
 * an ID. This prototype performs no compilation or migration: the backend
 * produces a failed report on the next eligible periodic tick.
 *
 * Consume <num_arg> values ending at <sp>, replacing them with the ID,
 * and return the new stack pointer. Admission failures are cleaned through
 * a stack error handler while the record remains on the global root list.
 */

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
    validate_source(request);
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
} /* v_update_blueprint() */

static svalue_t *
report_field (mapping_t *report, const char *name)

/* Return the writable value slot for key <name> in the fresh <report>.
 * The caller must already have rooted the partial report on the interpreter
 * stack. Keep the temporary key rooted there until insertion succeeds.
 * Raise an LPC out-of-memory error if mapping insertion returns NULL, so
 * the normal error unwinder releases both key and partial report. This
 * function never returns a NULL slot to its callers.
 */

{
    svalue_t *result;

    push_c_string(inter_sp, name);
    result = get_map_lvalue(report, inter_sp);
    if (!result)
        outofmem(sizeof(*result), "blueprint update report field");
    pop_stack();
    return result;
} /* report_field() */

svalue_t *
f_update_blueprint_result (svalue_t *sp)

/* LPC efun: mapping update_blueprint_result(int id)
 *
 * Return a fresh, non-consuming report for an existing request owned by
 * the caller, or any existing request when called by the master. Unknown,
 * expired, or unauthorized IDs raise an LPC error. Reports contain id,
 * origin, selection, status, candidate_generation, matched, updated,
 * already_current, destroyed, blueprint_updated, variable_changes, errors,
 * and completed_at. Pending unknowns are zero and arrays are empty.
 *
 * Nested arrays and mappings are independently allocated. The stored
 * terminal record contains no source or target references. Replace the ID
 * at <sp> with the mapping and return <sp>. Keep the partial report rooted
 * on the interpreter stack so allocation errors can unwind it safely.
 */

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
    put_number(report_field(report, "candidate_generation"), request->candidate_generation);
    put_number(report_field(report, "completed_at"), request->completed_at);
    field = report_field(report, "variable_changes");
    if (request->roots[UPDATE_SCHEMAS].type == T_POINTER)
    {
        push_svalue(&request->roots[UPDATE_SCHEMAS]);
        inter_sp = f_deep_copy(inter_sp);
        transfer_svalue_no_free(field, inter_sp);
        inter_sp--;
    }
    else
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
} /* f_update_blueprint_result() */

void
program_update_detach (void)

/* Detach the pending work chain for one periodic backend tick. Call once
 * at entry to the time_to_call_heart_beat block, before refreshing time or
 * running callbacks. The previous detached batch must already be empty.
 * No LPC code or allocating operation runs here.
 *
 * Moving queue links does not change ownership: every detached request
 * remains on the global requests root. Submissions after this boundary
 * stay on pending and cannot execute during the current tick.
 */

{
    assert(!batch);
    batch = pending;
    pending = NULL;
} /* program_update_detach() */

void
program_update_process (void)

/* Process the previously detached batch with a per-request backend error
 * boundary, then expire terminal reports. The backend must be idle, with
 * no active LPC evaluation, current_object cleared, current_time refreshed,
 * periodic flags reset, and next_call_out_cycle() already completed. Call
 * before the heartbeat rate/enable gate and callout dispatch.
 *
 * The global requests chain roots the batch and active request throughout
 * evaluation, including any later compiler callbacks. Reentrant submission
 * only appends to pending. Owner destruction marks a busy request canceled
 * instead of freeing it from under the active evaluation.
 *
 * Current finalization uses only non-reentrant native reference releases.
 * TODO: Before adding reentrant compiler/native cleanup, retain the busy
 * state and a GC-visible retirement root until release is fully complete.
 * The present busy-to-terminal transition is not such a retirement protocol.
 */

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
            /* Partial summaries are never exposed as completed evidence. */
            free_svalue(&active->roots[UPDATE_SCHEMAS]);
            put_number(&active->roots[UPDATE_SCHEMAS], 0);
            active->candidate_generation = 0;
        }
        else if (!active->canceled)
        {
            mark_start_evaluation();
            validate_source(active);
            validate_targets(active, MY_FALSE);
            /* Mapping allocations charge the submitting owner. This native
             * pass invokes no LPC and restores the idle backend context.
             */
            set_current_object(active->owner);
            describe_schemas(active);
            clear_current_object();
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
} /* program_update_process() */

void
program_update_owner_destructed (object_t *owner)

/* Cancel every request owned by the destructing <owner>, including cached
 * reports, and remove all non-owning owner links. A busy admission or active
 * backend request stays globally rooted with canceled set; its own error
 * handler or backend frame performs the eventual release. Other records
 * are synchronously freed. Call before <owner>'s storage can disappear.
 */

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
} /* program_update_owner_destructed() */

void
program_update_shutdown (void)

/* Free every remaining request and report during idle backend shutdown.
 * No admission handler or active request evaluation may still own a record.
 * Uses the same non-reentrant native teardown contract as free_request().
 */

{
    while (requests)
        free_request(requests);
} /* program_update_shutdown() */

void
program_update_cleanup (cleanup_t *context)

/* Include request-owned aggregate roots in the ordinary data-clean pass,
 * which compacts mapping hashes before the collector clears references.
 */
{
    program_update_request_t *request;
    for (request = requests; request; request = request->next)
        cleanup_vector(request->roots, UPDATE_NUM_ROOTS, context);
} /* program_update_cleanup() */

#ifdef GC_SUPPORT
void
program_update_clear_refs (void)

/* Clear GC references for every globally rooted request, its svalue roots,
 * and its independently retained program. This includes unpublished
 * admissions, pending work, detached/active work, and cached terminal data.
 * Called only during the collector's clear-reference phase.
 */

{
    program_update_request_t *request;
    for (request = requests; request; request = request->next)
    {
        clear_memory_reference(request);
        clear_ref_in_vector(request->roots, UPDATE_NUM_ROOTS);
        if (request->source_program)
            clear_program_ref(request->source_program, MY_TRUE);
    }
} /* program_update_clear_refs() */

void
program_update_count_refs (void)

/* Mark every request allocation and reconstruct its svalue and retained
 * program references during the collector's count-reference phase. Owner
 * reverse links are deliberately non-owning and must not be counted.
 */

{
    program_update_request_t *request;
    for (request = requests; request; request = request->next)
    {
        note_malloced_block_ref(request);
        count_ref_in_vector(request->roots, UPDATE_NUM_ROOTS);
        if (request->source_program)
            mark_program_ref(request->source_program);
    }
} /* program_update_count_refs() */
#endif /* GC_SUPPORT */

#ifdef DEBUG
void
program_update_count_extra_refs (void)

/* Count request-owned object, aggregate, and exact program references for
 * DEBUG reference verification. Include admission and detached/active work
 * through the global root list; do not count the non-owning owner links.
 */

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
} /* program_update_count_extra_refs() */
#endif /* DEBUG */
#endif /* USE_BLUEPRINT_UPDATE */
