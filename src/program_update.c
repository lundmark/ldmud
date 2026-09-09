/* Deferred blueprint update transactions and request ownership.
 * Terminal failure state is
 * reserved in the admission allocation. Schema diagnostics may allocate within
 * the backend recovery boundary, with partial summaries rooted in the request.
 */
#include "driver.h"
#ifdef USE_BLUEPRINT_UPDATE
#include <assert.h>
#include <stdio.h>
#include "program_update.h"
#include "program_schema.h"
#include "prolang.h"
#include "ptrtable.h"
#include "efuns.h"
#include "array.h"
#include "backend.h"
#include "exec.h"
#include "gcollect.h"
#include "interpret.h"
#include "main.h"
#include "lex.h"
#include "mapping.h"
#include "mstrings.h"
#include "object.h"
#include "simulate.h"
#include "simul_efun.h"
#include "stdstrings.h"
#include "svalue.h"
#include "swap.h"
#include "structs.h"
#include "i-eval_cost.h"
#include "xalloc.h"
#include "i-current_object.h"
#ifdef USE_PYTHON
#include "pkg-python.h"
#endif

void
program_dependency_init (program_dependency_t *link, void *handle,
                         enum program_dependency_kind kind)

/* Initialize an unpublished allocation, including GC sample handles. */

{
    link->object = NULL;
    link->next = NULL;
    link->prev = NULL;
    link->peer = NULL;
    link->handle = handle;
    link->kind = kind;
}

void
program_dependency_detach (program_dependency_t *link)

/* Unlink without allocation, callbacks, or counted-reference releases. */

{
    if (!link->object)
        return;
    *link->prev = link->next;
    if (link->next)
        link->next->prev = link->prev;
    link->object = NULL;
    link->next = NULL;
    link->prev = NULL;
}

void
program_dependency_attach (program_dependency_t *link, object_t *ob)

/* The handle already owns its endpoint. Inventory adds no reference. */

{
    assert(!link->object);
    if (!ob || ob->flags & O_DESTRUCTED)
        return;
    link->object = ob;
    link->next = ob->program_dependencies;
    link->prev = &ob->program_dependencies;
    if (link->next)
        link->next->prev = &link->next;
    ob->program_dependencies = link;
}

void
program_dependencies_clear (object_t *ob)

/* Drain while all allocations are still valid: at irreversible destruction,
 * and before GC clears marks. No release can reenter or invalidate a peer.
 */

{
    while (ob->program_dependencies)
    {
        program_dependency_t *link = ob->program_dependencies;
        if (link->peer)
            program_dependency_detach(link->peer);
        program_dependency_detach(link);
    }
}

#define BLUEPRINT_UPDATE_REPORT_TTL 300
#define BLUEPRINT_UPDATE_MAX_REPORTS 256

enum update_root
{
    UPDATE_ORIGIN, UPDATE_SCHEMAS, UPDATE_DIAGNOSTICS, UPDATE_SOURCE, UPDATE_TARGETS,
    UPDATE_BLUEPRINT_SCHEMA, UPDATE_BLUEPRINT_DEFAULTS,
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    UPDATE_TEST_VALUES,
#endif
    UPDATE_NUM_ROOTS
};
typedef struct program_update_variables_s
{
    struct program_update_variables_s *next;
    svalue_t object;              /* Pins the live owner through retirement. */
    program_t *old_program;       /* Preparation pin, then retired ownership. */
    svalue_t *values;             /* New slots, then the detached old block. */
    size_t num_values;            /* Initialized prefix still owned by record. */
    Bool snapshot;               /* Loaded-mode blueprint, never installed. */
} program_update_variables_t;

typedef struct program_update_request_s
{
    error_handler_t handler;
    struct program_update_request_s *next;
    struct program_update_request_s *owner_next;
    struct program_update_request_s *work_next;
    object_t *owner;              /* Non-owning; destruction unlinks it. */
    program_t *source_program;    /* Exact generation, independently pinned. */
    program_t *candidate;         /* Unpublished program, independently rooted. */
    program_update_variables_t *variables;
    svalue_t roots[UPDATE_NUM_ROOTS];
    p_int id;
    p_int candidate_generation;
    p_int matched, already_current, destroyed, updated;
    size_t num_diagnostics, num_blockers;
    size_t target_limit, scan_limit;
    schema_budget_t budget;
    const char *failure_code;
    char failure_message[2 * MAXPATHLEN + 256];
    time_t completed_at;
    Bool source_from_path;
    Bool source_has_clones;
    Bool explicit_selection;
    Bool terminal;
    Bool committed;              /* Immutable outcome, independent of cleanup. */
    Bool blueprint_updated;
    Bool busy;                   /* Admission or current backend evaluation. */
    Bool canceled;
} program_update_request_t;

/* Admission, detached batch, and terminal requests all stay on this root. */
static program_update_request_t *requests;
static program_update_request_t *pending;
static program_update_request_t *batch;
static program_update_request_t *active;
static unsigned int cleanup_depth;
/* Static storage outlives compiler handlers and every guarded program
 * release, including recovery after a suspended callback boundary.
 */
static stack_gap_guard_t preparation_guard;
static stack_gap_guard_t *previous_preparation_guard;
static Bool preparation_guard_active;
static p_int last_id;

static svalue_t *report_field(mapping_t *report, const char *name);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
static void migration_test_begin(void);
#endif

#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
static int report_test_countdown = -1;
static void
report_test_begin(const char *file)
{
    FILE *input = fopen(file, "r");
    report_test_countdown = -1;
    if (input)
    {
        if (fscanf(input, "%d", &report_test_countdown) != 1)
            report_test_countdown = -1;
        fclose(input);
    }
}
static Bool
report_test_fail(void)
{
    return report_test_countdown >= 0 && report_test_countdown-- == 0;
}
static void
report_test_step(void)
{
    if (report_test_fail())
        outofmemory("injected blueprint report allocation");
}
#define REPORT_TEST_NULL(allocation) (report_test_fail() ? NULL : (allocation))
#else
#define report_test_begin(file) ((void)0)
#define report_test_step() ((void)0)
#define REPORT_TEST_NULL(allocation) (allocation)
#endif

static time_t
report_time(void)
/* A private clock keeps expiry/eviction tests independent of backend time. */
{
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    FILE *input = fopen("requests-clock", "r");
    long value;
    if (input)
    {
        Bool valid = fscanf(input, "%ld", &value) == 1 && value > 0;
        fclose(input);
        if (valid) return (time_t)value;
    }
#endif
    return current_time;
}

#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
static int default_test_point, default_test_countdown;
static Bool default_test_literal, default_test_active;

static void
default_test_begin(void)
{
    FILE *input;
    default_test_point = default_test_countdown = 0;
    default_test_literal = default_test_active = MY_FALSE;
    if (!strcmp(get_txt(active->roots[UPDATE_ORIGIN].u.str), "/defaults_target"))
        input = fopen("defaults-fault", "r");
    else if (!strcmp(get_txt(active->roots[UPDATE_ORIGIN].u.str), "/requests_target"))
        input = fopen("requests-default-once", "r");
    else
        return;
    if (!input)
        return;
    if (fscanf(input, "%d %d", &default_test_point, &default_test_countdown) == 2
     && default_test_countdown >= 0)
        default_test_active = MY_TRUE;
    fclose(input);
}

Bool
program_update_default_test_fail(int point)
{
    /* Allocator sites are selected separately for literal construction and
     * schema/report ownership. Ordinary LPC callbacks have another guard.
     */
    if (point <= DEFAULT_TEST_CHAIN && !default_test_literal)
        point = point == DEFAULT_TEST_MAPPING ? DEFAULT_TEST_REPORT_MAPPING
              : point == DEFAULT_TEST_STRING ? DEFAULT_TEST_REPORT_STRING
              : point == DEFAULT_TEST_HASH ? DEFAULT_TEST_REPORT_HASH
              : point == DEFAULT_TEST_CHAIN ? DEFAULT_TEST_REPORT_CHAIN
              : DEFAULT_TEST_REPORT_ARRAY;
    if (!default_test_active || !active || !preparation_guard_active
     || get_stack_gap_guard() != &preparation_guard || default_test_point != point)
        return MY_FALSE;
    if (default_test_countdown)
    {
        default_test_countdown--;
        return MY_FALSE;
    }
    default_test_point = 0;
    return MY_TRUE;
}

void
program_update_default_test_scope(Bool literal)
{
    default_test_literal = literal;
}

void
program_update_default_test_pressure(int point)
{
    if (program_update_default_test_fail(point))
        test_stack_gap_failure();
}
#endif

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
retirement_test_checkpoint (program_update_request_t *request)
{
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING) && defined(GC_SUPPORT)
    FILE *input;
    int countdown;
    if (!request->committed || strcmp(get_txt(request->roots[UPDATE_ORIGIN].u.str), "/retire_target")) return;
    input = fopen("retirement-gc", "r");
    if (!input) return;
    if (fscanf(input, "%d", &countdown) != 1) countdown = 0;
    fclose(input);
    if (--countdown > 0)
    {
        input = fopen("retirement-gc", "w");
        if (input) { fprintf(input, "%d\n", countdown); fclose(input); }
        return;
    }
    remove("retirement-gc");
    size_t saved_array = max_array_size, saved_mapping = max_mapping_size, saved_keys = max_mapping_keys;
    int32 saved_eval = max_eval_cost, saved_file = max_file_xfer, saved_byte = max_byte_xfer;
    int32 saved_callouts = max_callouts, saved_use = use_eval_cost;
    int32 saved_cost = eval_cost, saved_assigned = assigned_eval_cost;
    p_int saved_memory = max_memory;
    int saved_privilege = malloc_privilege;
    /* This call is AFTER free_svalue returned and its queue fully drained.
     * The partial old block, all later records, active/batch/pending requests
     * remain on the ordinary roots. No LPC evaluation or stack handler exists.
     */
    assert(request->busy && active == request);
    assert(batch && pending && request->variables->num_values > 0);
    clear_state();
    garbage_collection();
    max_array_size = saved_array; max_mapping_size = saved_mapping; max_mapping_keys = saved_keys;
    max_eval_cost = saved_eval; max_file_xfer = saved_file; max_byte_xfer = saved_byte;
    max_callouts = saved_callouts; use_eval_cost = saved_use;
    eval_cost = saved_cost; assigned_eval_cost = saved_assigned;
    max_memory = saved_memory; malloc_privilege = saved_privilege;
    debug_message("BLUEPRINT_RETIREMENT_NATIVE_GC: partial retirement and request queues remain rooted.\n");
#else
    (void)request;
#endif
}

static void
release_variables (program_update_request_t *request)

/* Dispose staged or retired blocks with persistent ownership/cursors. All
 * records remain linked until all variable slots have drained. Native
 * pressure is guarded by the caller; each free_svalue queue drains before
 * control can reach a GC checkpoint. Python finalizers can reenter LPC only
 * through its complete secure error boundary, with that guard suspended.
 *
 * Staging rollback is deliberately narrower than postcommit retirement.
 * All old variable blocks and every source/target object remain owned and
 * unchanged until ALL prepared references have been released. An arbitrary
 * retained/shared value (including a descendant in a projected range) still
 * has that old owner; only newly materialized native literals can reach zero.
 * Therefore these releases cannot finalize Python/LW objects or call LPC.
 * Postcommit these values may instead be the final arbitrary owner: the
 * whole cohort and immutable outcome have already been installed, and busy
 * retains the request/family reservation throughout callback-capable release.
 */

{
    program_update_variables_t *stage;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    if (request->committed && !strcmp(get_txt(request->roots[UPDATE_ORIGIN].u.str), "/retire_target")
     && remove("retirement-pressure") == 0)
    {
        test_stack_gap_failure();
        debug_message("BLUEPRINT_RETIREMENT_PRESSURE: native disposal guard latched; committed outcome retained.\n");
    }
#endif
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    free_svalue(&request->roots[UPDATE_TEST_VALUES]);
    put_number(&request->roots[UPDATE_TEST_VALUES], 0);
#endif
    for (stage = request->variables; stage; stage = stage->next)
    {
        while (stage->num_values)
        {
            free_svalue(stage->values + --stage->num_values);
            retirement_test_checkpoint(request);
        }
        if (stage->values) xfree(stage->values);
        stage->values = NULL;
    }
    while ((stage = request->variables))
    {
        free_svalue(&stage->object);
        if (stage->old_program)
        {
            program_t *old = stage->old_program;
            stage->old_program = NULL;
            free_prog(old, MY_TRUE);
        }
        request->variables = stage->next;
        xfree(stage);
    }
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    if (request == active && !strcmp(get_txt(request->roots[UPDATE_ORIGIN].u.str), "/fault_target"))
    {
        FILE *output = fopen("migration-released", "w");
        if (output) { fputs("1\n", output); fclose(output); }
    }
#endif
} /* release_variables() */

static void
variables_cleanup (error_handler_t *handler)

/* errorf() resets the value stack before invoking master:runtime_error().
 * Retire staging at that boundary, while no callback has yet had a chance
 * to overwrite an old variable or destroy its owner. The backend setjmp is
 * too late to establish that ownership guarantee.
 */

{
    program_update_request_t *request = (program_update_request_t *)handler;
    stack_gap_guard_t *previous = set_stack_gap_guard(&preparation_guard);
    if (!request->committed) release_variables(request);
    set_stack_gap_guard(previous);
} /* variables_cleanup() */

static void
release_inputs (program_update_request_t *request)

/* Release and zero the source and fixed target references of <request>,
 * then release its independently pinned program. The origin and reserved
 * terminal record survive. Repeated calls are harmless after completion.
 *
 * The caller keeps the record busy and globally rooted, with a native
 * stack-gap guard installed. Consumed fields are invalidated before any
 * finalizer; the free queue must drain before a native GC checkpoint.
 */

{
    int i;
    release_variables(request);
    for (i = UPDATE_SOURCE; i < UPDATE_NUM_ROOTS; i++)
    {
        free_svalue(&request->roots[i]);
        put_number(&request->roots[i], 0);
    }
    if (request->candidate)
    {
        program_t *candidate = request->candidate;
        request->candidate = NULL;
        free_prog(candidate, MY_TRUE);
    }
    if (request->source_program)
    {
        program_t *source = request->source_program;
#if defined(TRACE_CODE) && defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
        Bool trace_test = request->committed && !strcmp(get_txt(request->roots[UPDATE_ORIGIN].u.str), "/trace_target");
        if (trace_test) assert(source->ref == 1);
#endif
        request->source_program = NULL;
        free_prog(source, MY_TRUE);
#if defined(TRACE_CODE) && defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
        if (trace_test) program_update_trace_test(source, MY_TRUE);
#endif
    }
} /* release_inputs() */

static void
free_request (program_update_request_t *request)

/* Release <request> while it remains busy and globally rooted, then unlink
 * its final root in a nonreentrant tail. The caller must
 * ensure no stack handler or active evaluation will use it afterwards.
 *
 * The native guard prevents disposal pressure from abandoning the global
 * free_svalue queue. Python-to-LPC calls suspend it within their own complete
 * error boundary; ordinary callback errors cannot unwind native disposal.
 */

{
    program_update_request_t **link;
    stack_gap_guard_t guard = { MY_FALSE };
    stack_gap_guard_t *previous = set_stack_gap_guard(&guard);
    request->busy = MY_TRUE;
    unlink_owner(request);
    unlink_work(&pending, request);
    unlink_work(&batch, request);
    release_inputs(request);
    free_svalue(&request->roots[UPDATE_ORIGIN]);
    free_svalue(&request->roots[UPDATE_SCHEMAS]);
    free_svalue(&request->roots[UPDATE_DIAGNOSTICS]);
    link = &requests;
    while (*link != request)
        link = &(*link)->next;
    *link = request->next;
    xfree(request);
    set_stack_gap_guard(previous);
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
 * Eligible terminal reports own only native immutable summary values;
 * callback-capable input retirement finishes before busy is cleared.
 */

{
    program_update_request_t *request, *next, *oldest;
    size_t count = 0;
    time_t now = report_time();
    if (cleanup_depth) return;
    for (request = requests; request; request = next)
    {
        next = request->next;
        if (!request->terminal || request->busy)
            continue;
        if (now - request->completed_at >= BLUEPRINT_UPDATE_REPORT_TTL)
            free_request(request);
        else
            count++;
    }
    while (count > BLUEPRINT_UPDATE_MAX_REPORTS)
    {
        oldest = NULL;
        for (request = requests; request; request = request->next)
            if (request->terminal && !request->busy
             && (!oldest || request->completed_at < oldest->completed_at
              || (request->completed_at == oldest->completed_at
               && request->id < oldest->id)))
                oldest = request;
        free_request(oldest);
        count--;
    }
} /* expire_reports() */

static void
request_failure(program_update_request_t *request, const char *code,
                const char *message)
{
    request->failure_code = code;
    snprintf(request->failure_message, sizeof(request->failure_message), "%s", message);
}

static void
request_error(program_update_request_t *request, const char *code, const char *message)
{
    if (request) request_failure(request, code, message);
    errorf("update_blueprint(): %s\n", message);
}

static void
object_error(program_update_request_t *request, object_t *object,
             const char *code, const char *reason)
{
    char message[2 * MAXPATHLEN + 256];
    snprintf(message, sizeof(message), "%s: %s",
             object ? get_txt(object->name) : "<missing>", reason);
    request_error(request, code, message);
}

static Bool
same_family (object_t *object, string_t *origin)

/* Return true if <object>'s canonical load name equals <origin>.
 * Both arguments are borrowed references; no lookup or loading occurs.
 */

{
    return object->load_name && mstreq(object->load_name, origin);
} /* same_family() */

static void
validate_object (object_t *object, string_t *origin, Bool source,
                 Bool check_family, program_update_request_t *request)

/* Require <object> to be a supported, live member of canonical family
 * <origin>. With <source> true it must be the actual ordinary blueprint;
 * otherwise a same-family clone is also accepted. Reject special roles,
 * virtual/replaced objects, and, with <check_family>, pending replace_program
 * conflicts. Initial source-path execution checks only the captured source;
 * its family is checked after compiler callbacks and complete count discovery.
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
        object_error(request, object, "UNSUPPORTED_OBJECT", "unsupported object.");
    /* Backup names may include a leading slash or .c suffix. Resolve only
     * an existing identity; admission must never load a sefun implicitly.
     */
    if (simul_efun_vector)
    {
        size_t i;

        for (i = 1; i < VEC_SIZE(simul_efun_vector); i++)
            if (simul_efun_vector->item[i].type == T_STRING
             && find_object(simul_efun_vector->item[i].u.str) == object)
                object_error(request, object, "SPECIAL_OBJECT", "backup simul-efun object.");
    }
    if (source && (object->flags & O_CLONE))
        object_error(request, object, "SOURCE_UNSUPPORTED", "source must be a blueprint.");
    if (!same_family(object, origin))
        object_error(request, object, "UNRELATED_TARGET", "unrelated target.");
    if (object->flags & O_SWAPPED)
        if (load_ob_from_swap(object) < 0)
            object_error(request, object, "SWAP_FAILED", "cannot unswap object.");
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
        object_error(request, object, "UNSUPPORTED_OBJECT", "virtual or replaced object.");
    if (check_family)
        for (replacement = obj_list_replace; replacement; replacement = replacement->next)
            if (same_family(replacement->ob, origin))
                object_error(request, object, "REPLACEMENT_PENDING", "pending replace_program conflict.");
} /* validate_object() */

static void
validate_source (program_update_request_t *request, Bool check_family)

/* Require <request>'s captured source to remain live and supported with
 * its exact pinned program. Admission and execution share this check; a
 * new blueprint at the same pathname cannot replace the captured identity.
 * <check_family> additionally rejects queued family replacement work.
 * Return normally on success, otherwise raise an LPC error.
 */

{
    object_t *source;

    if (request->roots[UPDATE_SOURCE].type != T_OBJECT)
        request_error(request, "SOURCE_INVALIDATED", "Captured source was destructed.");
    source = request->roots[UPDATE_SOURCE].u.ob;
    if (source->flags & O_DESTRUCTED)
        request_error(request, "SOURCE_INVALIDATED", "Captured source was destructed.");
    validate_object(source, request->roots[UPDATE_ORIGIN].u.str, MY_TRUE, check_family, request);
    if (source->prog != request->source_program)
        request_error(request, "SOURCE_INVALIDATED", "Captured source program changed.");
} /* validate_source() */

Bool
program_update_compilation_valid (void)

/* Revalidate identities immediately after a compiler callback without
 * allocating, unswapping, or raising an error through parser-owned values.
 * The pinned program cannot be swapped while this request owns it. Full
 * family/target validation still runs after normal compiler cleanup.
 */

{
    object_t *source;

    if (!active || active->canceled || !active->owner
     || (active->owner->flags & O_DESTRUCTED)
     || active->roots[UPDATE_SOURCE].type != T_OBJECT)
        return MY_FALSE;
    source = active->roots[UPDATE_SOURCE].u.ob;
    if (!(source->flags & (O_DESTRUCTED | O_REPLACED | O_SHADOW | O_CLONE))
     && source->prog == active->source_program
     && active->source_program->blueprint == source
     && same_family(source, active->roots[UPDATE_ORIGIN].u.str))
        return MY_TRUE;
    request_failure(active, "SOURCE_INVALIDATED", "Compiler callback invalidated the captured source.");
    return MY_FALSE;
} /* program_update_compilation_valid() */

static void
request_bytes(program_update_request_t *request, size_t bytes)
{
    if (bytes >= request->budget.bytes)
    {
        request_failure(request, "REPORT_SIZE_LIMIT", "Request/report storage limit exceeded; evidence is incomplete.");
        errorf("update_blueprint(): request/report storage limit exceeded.\n");
    }
    request->budget.bytes -= bytes;
}

void
program_update_compile_failure(const char *code, const char *message)
/* Compiler callbacks and resource failures may call this without allocating.
 * Preserve the more specific identity failure from callback revalidation.
 */
{
    if (active && (!strcmp(active->failure_code, "PREPARATION_PENDING")
                || !strcmp(active->failure_code, "COMPILE_FAILED")))
        request_failure(active, code, message);
}

void
program_update_compile_diagnostic(string_t *file, int line, Bool warning,
                                  string_t *message)
/* The compiler still owns these strings. Publish every partial container in
 * the request before another allocation, then take independent string refs.
 */
{
    svalue_t *root = &active->roots[UPDATE_DIAGNOSTICS];
    mapping_t *row;
    const char *saved_code = active->failure_code;
    char saved_message[sizeof(active->failure_message)];
    memcpy(saved_message, active->failure_message, sizeof(saved_message));
    if (!active->num_diagnostics) report_test_begin("requests-diagnostic-fault");
    request_failure(active, "REPORT_ALLOCATION_FAILED",
                    "Compiler diagnostic report allocation failed; evidence is incomplete.");
    if (active->num_diagnostics == BLUEPRINT_UPDATE_MAX_DIAGNOSTICS)
    {
        request_failure(active, "COMPILE_DIAGNOSTIC_LIMIT", "Compiler diagnostic limit exceeded; evidence is incomplete.");
        errorf("update_blueprint(): compiler diagnostic limit exceeded.\n");
    }
    request_bytes(active, 4096 + mstrsize(file) + mstrsize(message));
    if (root->type != T_POINTER)
    {
        request_bytes(active, sizeof(vector_t) + BLUEPRINT_UPDATE_MAX_DIAGNOSTICS * sizeof(svalue_t));
        report_test_step();
        put_array(root, allocate_array(BLUEPRINT_UPDATE_MAX_DIAGNOSTICS));
    }
    row = REPORT_TEST_NULL(allocate_mapping(5, 1));
    if (!row) outofmemory("blueprint compiler diagnostic");
    put_mapping(&root->u.vec->item[active->num_diagnostics], row);
    put_c_string(report_field(row, "code"), warning ? "COMPILE_WARNING" : "COMPILE_ERROR");
    put_ref_string(report_field(row, "file"), file);
    put_number(report_field(row, "line"), line);
    put_number(report_field(row, "warning"), warning);
    put_ref_string(report_field(row, "message"), message);
    active->num_diagnostics++;
    request_failure(active, saved_code, saved_message);
}

static void
count_targets(program_update_request_t *request)
/* Count the complete execution cohort before strict survivor validation.
 * Commit counters only after bounded discovery finishes, so a resource
 * failure never presents a prefix as the complete selected population.
 */
{
    vector_t *targets = request->explicit_selection
                        ? request->roots[UPDATE_TARGETS].u.vec : NULL;
    program_t *candidate = request->source_from_path
                           ? request->candidate : request->source_program;
    object_t *object;
    size_t scanned = 0;
    p_int matched = 0, destroyed = 0, current = 0;
    Bool source_has_clones = MY_FALSE;
    request->matched = request->already_current = request->destroyed = 0;
    for (object = targets ? NULL : obj_list; targets || object;
         object = targets ? NULL : object->next_all)
    {
        if (targets)
        {
            if (scanned == VEC_SIZE(targets)) break;
            object = targets->item[scanned].type == T_OBJECT
                     ? targets->item[scanned].u.ob : NULL;
        }
        if (++scanned > request->scan_limit)
            request_error(request, "SELECTION_SCAN_LIMIT",
                          "Object scan limit exceeded; selection counts are unavailable.");
        if (!object || object->flags & O_DESTRUCTED)
        {
            if (targets) destroyed++;
            continue;
        }
        if (!(object->flags & O_CLONE)
         || (!targets && !same_family(object, request->roots[UPDATE_ORIGIN].u.str)))
            continue;
        matched++;
        if (object->prog == request->source_program) source_has_clones = MY_TRUE;
        if (object->prog == candidate) current++;
    }
    request->matched = matched;
    request->already_current = current;
    request->destroyed = destroyed;
    request->source_has_clones = source_has_clones;
}

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
        request_error(request, "REQUEST_SIZE_LIMIT", "retained storage limit exceeded.");
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
        if (++scanned > request->scan_limit)
            request_error(request, "SELECTION_SCAN_LIMIT", "object scan limit exceeded.");
        if (object->flags & O_DESTRUCTED)
        {
            if (admission)
                errorf("update_blueprint(): target was destructed during authorization.\n");
            continue;
        }
        if (!targets && (!(object->flags & O_CLONE) || !same_family(object, origin)))
            continue;
        validate_object(object, origin, MY_FALSE, MY_TRUE, request);
        if (!(object->flags & O_CLONE))
            continue;
        if (++count > request->target_limit)
            request_error(request, "TARGET_LIMIT", "target limit exceeded.");
        slots += object->prog->num_variables;
        if (slots > BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS
         || slots > (BLUEPRINT_UPDATE_MAX_BYTES - retained) / sizeof(svalue_t))
            request_error(request, "VARIABLE_STORAGE_LIMIT", "variable storage limit exceeded.");
    }
} /* validate_targets() */

static Bool
validate_runtime_object (program_update_request_t *request, object_t *ob)

/* Called at the final compatibility boundary, after schema diagnostics.
 * No callbacks or allocation occur while consulting the weak inventories.
 */

{
    program_dependency_t *link;
    const char *code = NULL, *reason = NULL;
    char message[2 * MAXPATHLEN + 256];

    link = ob->program_dependencies;
    if (link)
    {
        if (link->kind == PROGRAM_DEPENDENCY_COROUTINE)
        {
            code = "LIVE_COROUTINE";
            reason = "unfinished coroutine depends on the current program.";
        }
        else
        {
            code = "LIVE_CLOSURE";
            reason = "live closure depends on the current program.";
        }
    }
#ifdef USE_PYTHON
    if (!code && python_program_has_handles(ob))
    {
        code = "PYTHON_HANDLE";
        reason = "live Python handle depends on the current program.";
    }
#endif
    if (!code)
        return MY_TRUE;
    snprintf(message, sizeof(message), "%s: %s", get_txt(ob->name), reason);
    /* The schema pass completed. Do not unwind through the preparation
     * error handler, which correctly discards incomplete schema evidence.
     */
    request_failure(request, code, message);
    return MY_FALSE;
}

static void
validate_runtime_dependencies (program_update_request_t *request)

/* Only programs that would change need compatibility checks. Object-source
 * mode leaves its blueprint and already-current clones unchanged. Selection
 * validity and complete counts have already been established after hooks.
 */

{
    object_t *ob;
    program_t *candidate = request->source_from_path ? request->candidate
                                                    : request->source_program;
    vector_t *targets = request->explicit_selection
                       ? request->roots[UPDATE_TARGETS].u.vec : NULL;
    size_t index = 0;

    if (request->source_from_path)
        if (!validate_runtime_object(request, request->roots[UPDATE_SOURCE].u.ob))
            return;
    for (ob = targets ? NULL : obj_list; targets || ob;
         ob = targets ? NULL : ob->next_all)
    {
        if (targets)
        {
            if (index == VEC_SIZE(targets)) break;
            svalue_t *value = &targets->item[index++];
            ob = value->type == T_OBJECT ? value->u.ob : NULL;
        }
        if (!ob || ob->flags & O_DESTRUCTED || !(ob->flags & O_CLONE)
         || ob->prog == candidate
         || (!targets && !same_family(ob, request->roots[UPDATE_ORIGIN].u.str)))
            continue;
        if (!validate_runtime_object(request, ob))
            return;
    }
}

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
    schema_budget_t *budget = &request->budget;
    Bool compatible = MY_TRUE;
    size_t capacity = targets ? VEC_SIZE(targets) : 0;
    object_t *object;
    vector_t *trimmed;
    program_t *candidate = request->source_from_path
                           ? request->candidate : request->source_program;

    request->candidate_generation = candidate->schema_generation;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    program_schema_test_rtt(candidate);
    if (program_update_default_test_fail(99))
        program_schema_test_defaults(request->source_program,
                                     &request->roots[UPDATE_BLUEPRINT_SCHEMA]);
#endif
    if (request->source_from_path)
    {
        svalue_t *defaults = &request->roots[UPDATE_BLUEPRINT_DEFAULTS];
        size_t slots = candidate->num_variables;
        size_t bytes;
        if (slots > (SIZE_MAX - sizeof(vector_t)) / sizeof(svalue_t))
            errorf("update_blueprint(): source defaults allocation overflow.\n");
        bytes = sizeof(vector_t) + slots * sizeof(svalue_t);
        request_bytes(request, bytes);
        put_array(defaults, allocate_array(candidate->num_variables));
        if (stack_gap_guard_failed())
            errorf("update_blueprint(): memory pressure preparing source defaults.\n");
        if (!program_schema_compare(request->source_program, candidate,
                                    &request->roots[UPDATE_BLUEPRINT_SCHEMA],
                                    budget, defaults->u.vec, MY_TRUE))
            compatible = MY_FALSE;
    }
    if (!targets)
    {
        for (object = obj_list; object; object = object->next_all)
        {
            if (++scanned > request->scan_limit)
                errorf("update_blueprint(): schema scan limit exceeded.\n");
            if (!(object->flags & O_DESTRUCTED) && (object->flags & O_CLONE)
             && same_family(object, request->roots[UPDATE_ORIGIN].u.str)
             && object->prog != candidate)
                capacity++;
        }
        scanned = 0;
    }
    if (request->source_from_path) capacity++;
    request_bytes(request, sizeof(vector_t) + capacity * sizeof(svalue_t));
    put_array(root, allocate_array(capacity));
    if (request->source_from_path)
    {
        svalue_t *row = &root->u.vec->item[count++];
        svalue_t *added;
        if (request->source_has_clones)
        {
            if (!program_schema_compare(request->source_program, candidate, row, budget,
                                        request->roots[UPDATE_BLUEPRINT_DEFAULTS].u.vec, MY_FALSE))
                compatible = MY_FALSE;
        }
        else if (!program_schema_compare_blueprint(request->source_program, candidate,
                     row, budget, &request->roots[UPDATE_BLUEPRINT_SCHEMA]))
            compatible = MY_FALSE;
        put_number(report_field(row->u.map, "blueprint"), 1);
        added = report_field(request->roots[UPDATE_BLUEPRINT_SCHEMA].u.map, "added");
        assign_svalue_no_free(report_field(row->u.map, "blueprint_defaults"), added);
        assign_svalue_no_free(report_field(row->u.map, "blueprint_blockers"),
                             report_field(request->roots[UPDATE_BLUEPRINT_SCHEMA].u.map, "blockers"));
    }
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
        if (++scanned > request->scan_limit)
            errorf("update_blueprint(): schema scan limit exceeded.\n");
        if (object->flags & O_DESTRUCTED)
            continue;
        if (!targets && (!(object->flags & O_CLONE)
                     || !same_family(object, request->roots[UPDATE_ORIGIN].u.str)))
            continue;
        if (!(object->flags & O_CLONE) || object->prog == candidate)
            continue;
        for (previous = 0; previous < count; previous++)
        {
            svalue_t *generation;
            if (++work > request->scan_limit)
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
        if (count == request->target_limit + (request->source_from_path ? 1 : 0))
            errorf("update_blueprint(): schema generation limit exceeded.\n");
        if (!program_schema_compare(object->prog, candidate,
                               &root->u.vec->item[count++], budget,
                               request->source_from_path
                                   ? request->roots[UPDATE_BLUEPRINT_DEFAULTS].u.vec : NULL,
                               MY_FALSE))
            compatible = MY_FALSE;
        if (!budget->bytes)
            break;
    }
    if (!compatible)
        request_failure(request, budget->bytes ? "SCHEMA_INCOMPATIBLE" : "REPORT_SIZE_LIMIT",
                        budget->bytes ? "Blueprint schema or defaults are incompatible."
                                      : "Request/report storage limit exceeded; evidence is incomplete.");
    trimmed = slice_array(root->u.vec, 0, count - 1);
    free_svalue(root);
    put_array(root, trimmed);
    for (size_t i = 0; i < count; i++)
    {
        mapping_t *generation = root->u.vec->item[i].u.map;
        p_int old_generation = report_field(generation, "old_generation")->u.number;
        for (int role = 0; role < (request->source_from_path && i == 0 ? 2 : 1); role++)
        {
            vector_t *blocks = report_field(generation, role ? "blueprint_blockers" : "blockers")->u.vec;
            request_bytes(request, VEC_SIZE(blocks) * (512 + sizeof(svalue_t)));
            for (size_t j = 0; j < VEC_SIZE(blocks); j++)
            {
                mapping_t *block = blocks->item[j].u.map;
                if (!strcmp(get_txt(report_field(block, "code")->u.str), "SCHEMA_DIAGNOSTIC_LIMIT"))
                    request_failure(request, "SCHEMA_DIAGNOSTIC_LIMIT",
                                    "Schema diagnostic limit exceeded; evidence is incomplete.");
                put_number(report_field(block, "old_generation"), old_generation);
                put_number(report_field(block, "candidate_generation"), candidate->schema_generation);
                put_c_string(report_field(block, "role"), role ? "blueprint" : "generation");
                request->num_blockers++;
            }
        }
    }
} /* describe_schemas() */

static mapping_t *
variable_schema (program_update_request_t *request, program_t *old)
{
    vector_t *schemas = request->roots[UPDATE_SCHEMAS].u.vec;
    for (size_t i = 0; i < VEC_SIZE(schemas); i++)
    {
        if (!request->budget.work)
            errorf("update_blueprint(): variable generation work limit exceeded.\n");
        request->budget.work--;
        if (report_field(schemas->item[i].u.map, "old_generation")->u.number
             == old->schema_generation)
            return schemas->item[i].u.map;
    }
    errorf("update_blueprint(): variable generation plan unavailable.\n");
    return NULL;
}

static program_update_variables_t *
allocate_variables (program_update_request_t *request, object_t *object,
                    program_t *candidate, Bool snapshot)
{
    program_update_variables_t *stage;
    size_t count = candidate->num_variables;
    size_t old_count = object->prog->num_variables;

    if (count > request->budget.slots || old_count > request->budget.slots - count
     || count > SSIZE_MAX / sizeof(svalue_t) || old_count > SSIZE_MAX / sizeof(svalue_t))
        request_error(request, "VARIABLE_STORAGE_LIMIT", "migration variable slot limit exceeded.");
    if (count > request->budget.work)
        errorf("update_blueprint(): variable initialization work limit exceeded.\n");
    request->budget.work -= count;
    request->budget.slots -= count + old_count;
    request_bytes(request, sizeof(*stage));
    request_bytes(request, count * sizeof(svalue_t));
    request_bytes(request, old_count * sizeof(svalue_t));
    MIGRATION_TEST_STEP();
    stage = xalloc(sizeof(*stage));
    if (!stage) outofmemory("blueprint variable plan");
    *stage = (program_update_variables_t){ .next = request->variables, .snapshot = snapshot };
    request->variables = stage;
    put_ref_object(&stage->object, object, "blueprint variable staging");
    stage->old_program = object->prog;
    reference_prog(stage->old_program, "blueprint variable staging");
    MIGRATION_TEST_STEP();
    if (count)
    {
        stage->values = xalloc(count * sizeof(svalue_t));
        if (!stage->values) outofmemory("blueprint variable block");
        for (size_t i = 0; i < count; i++)
            stage->values[i].type = T_INVALID;
        stage->num_values = count;
    }
    MIGRATION_TEST_STEP();
    return stage;
}

static Bool
same_variable_value (const svalue_t *left, const svalue_t *right)
{
    if (left->type != right->type) return MY_FALSE;
    switch (left->type)
    {
    case T_INVALID:
        return MY_TRUE;
    case T_NUMBER:
        return left->u.number == right->u.number;
    case T_FLOAT:
#ifdef FLOAT_FORMAT_2
        return !memcmp(&left->u.float_number, &right->u.float_number, sizeof(double));
#else
        return left->u.mantissa == right->u.mantissa && left->x.exponent == right->x.exponent;
#endif
    case T_CLOSURE:
    case T_LVALUE:
    case T_SYMBOL:
    case T_QUOTED_ARRAY:
#ifdef USE_PYTHON
    case T_PYTHON:
#endif
        if (left->x.generic != right->x.generic) return MY_FALSE;
        /* FALLTHROUGH */
    default:
        return left->u.generic == right->u.generic;
    }
}

static void
prepare_variables (program_update_request_t *request)

/* Prepare the complete source first: a clone skipping generations may need
 * a shared addition that is already retained in this source generation.
 * Loaded mode captures the actual blueprint values, including protected
 * cells, and rechecks them at the final boundary. No program/block is installed.
 */

{
    object_t *source = request->roots[UPDATE_SOURCE].u.ob;
    program_t *candidate = request->source_from_path ? request->candidate : request->source_program;
    program_update_variables_t *blueprint = allocate_variables(request, source, candidate,
                                                              !request->source_from_path);
    vector_t *targets = request->explicit_selection ? request->roots[UPDATE_TARGETS].u.vec : NULL;
    object_t *object;
    size_t index = 0;

    if (request->source_from_path)
        program_schema_prepare_variables(source->prog, candidate, variable_schema(request, source->prog),
            source->variables, blueprint->values, NULL, MY_TRUE, &request->budget);
    else
        for (size_t i = 0; i < blueprint->num_values; i++)
        {
            if (!request->budget.work)
                errorf("update_blueprint(): blueprint capture work limit exceeded.\n");
            request->budget.work--;
            assign_update_svalue_no_free(blueprint->values + i, source->variables + i);
            MIGRATION_TEST_STEP();
        }
    for (object = targets ? NULL : obj_list; targets || object;
         object = targets ? NULL : object->next_all)
    {
        program_update_variables_t *stage;
        if (targets)
        {
            if (index == VEC_SIZE(targets)) break;
            object = targets->item[index].type == T_OBJECT ? targets->item[index].u.ob : NULL;
            index++;
        }
        if (!request->budget.work)
            errorf("update_blueprint(): migration selection work limit exceeded.\n");
        request->budget.work--;
        if (!object || object->flags & O_DESTRUCTED || !(object->flags & O_CLONE)
         || object->prog == candidate
         || (!targets && !same_family(object, request->roots[UPDATE_ORIGIN].u.str)))
            continue;
        stage = allocate_variables(request, object, candidate, MY_FALSE);
        program_schema_prepare_variables(object->prog, candidate, variable_schema(request, object->prog),
            object->variables, stage->values, blueprint->values, MY_FALSE, &request->budget);
    }
    validate_source(request, MY_TRUE);
    validate_targets(request, MY_FALSE);
    validate_runtime_dependencies(request);
    if (!request->source_from_path)
        for (size_t i = 0; i < blueprint->num_values; i++)
        {
            if (!request->budget.work)
                errorf("update_blueprint(): blueprint validation work limit exceeded.\n");
            request->budget.work--;
            if (!same_variable_value(blueprint->values + i, source->variables + i))
                request_error(request, "SOURCE_CHANGED", "loaded blueprint values changed during preparation.");
        }
} /* prepare_variables() */

static void
prepare_publication (program_update_request_t *request)

/* All allocations and swap IO precede the irreversible stores. The candidate
 * keeps its inherited graph alive, while each old generation remains pinned
 * by its preparation record. Removing these swap images is safe on rollback.
 */

{
    program_t *candidate = request->source_from_path ? request->candidate : request->source_program;
    for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
    {
        program_t *old = stage->old_program;
        if (old->swap_num != -1 && !old->line_numbers && !load_line_numbers_from_swap(old))
            outofmemory("blueprint publication line numbers");
        remove_prog_swap(old, MY_TRUE);
    }
    if (request->source_from_path)
    {
        assert(!candidate->blueprint && candidate->swap_num == -1);
        for (size_t i = 0; i < candidate->num_structs; i++)
            if (candidate->struct_defs[i].inh == STRUCT_INH_LOCAL)
                assert(candidate->struct_defs[i].type && candidate->struct_defs[i].type->prog_id);
    }
    if (preparation_guard.failed)
        errorf("update_blueprint(): memory pressure preparing publication.\n");
    request->completed_at = report_time();
#if defined(TRACE_CODE) && defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    if (!strcmp(get_txt(request->roots[UPDATE_ORIGIN].u.str), "/trace_target"))
        program_update_trace_test(request->source_program, MY_FALSE);
#endif
}

static void
commit_variables (program_update_request_t *request)

/* The complete transaction: counted-reference/pointer stores only. Records
 * allocated during preparation take ownership of the displaced blocks and
 * programs. No release, allocation, callback or collection belongs here.
 */

{
    program_t *candidate = request->source_from_path ? request->candidate : request->source_program;
    if (request->source_from_path)
    {
        candidate->blueprint = request->roots[UPDATE_SOURCE].u.ob;
        candidate->blueprint->ref++;
        for (size_t i = 0; i < candidate->num_structs; i++)
            if (candidate->struct_defs[i].inh == STRUCT_INH_LOCAL)
                struct_publish_type(candidate->struct_defs[i].type);
    }
    for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
    {
        object_t *ob;
        svalue_t *old_values;
        size_t old_count;
        if (stage->snapshot) continue;
        ob = stage->object.u.ob;
        old_values = ob->variables;
        old_count = stage->old_program->num_variables;
#ifdef TRACE_CODE
        invalidate_program_trace(stage->old_program);
#endif
        /* Drop the duplicate preparation pin without releasing the program;
         * the record now owns the reference formerly held by the object.
         */
        assert(stage->old_program->ref >= 2);
        stage->old_program->ref--;
        candidate->ref++;
        ob->prog = candidate;
        ob->variables = stage->values;
        stage->values = old_values;
        stage->num_values = old_count;
#ifdef DEBUG
        ob->extra_num_variables = candidate->num_variables;
#endif
        tot_alloc_object_size += ((long)candidate->num_variables - (long)old_count) * (long)sizeof(svalue_t);
        if (ob->flags & O_CLONE) request->updated++;
        else request->blueprint_updated = MY_TRUE;
    }
    /* Freeze the outcome before any old value can run a finalizer. The family
     * remains reserved until busy is cleared after all retirement is done.
     */
    request->committed = MY_TRUE;
    request->terminal = MY_TRUE;
}

#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
#include "../test/t-blueprint-migration/native.inc"
#endif

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
        if ((request->terminal && !request->busy) || request == self)
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
 * an ID. Admission performs no compilation or migration. The backend
 * prepares a candidate and reports failure before migration on the next
 * eligible periodic tick.
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
    size_t i, unique = 0;
    inter_sp = sp;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    if (active && active->committed)
        assert(!get_stack_gap_guard()); /* Entire LPC callback/error boundary. */
#endif
    expire_reports();
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    if (remove("requests-last-id") == 0)
        last_id = PINT_MAX;
#endif
    if (!get_current_object() || get_current_object()->flags & O_DESTRUCTED)
        errorf("update_blueprint(): caller has been destructed.\n");
    if (last_id == PINT_MAX)
        errorf("update_blueprint(): request identifiers exhausted.\n");
    if (num_arg == 2 && arg[1].type == T_NUMBER && arg[1].u.number)
        errorf("update_blueprint(): selection must be zero or an object array.\n");
    source = arg->type == T_OBJECT ? arg->u.ob : find_object(arg->u.str);
    if (!source)
        errorf("update_blueprint(): source blueprint is not loaded.\n");
    validate_object(source, source->load_name, MY_TRUE, MY_TRUE, NULL);
    request = xalloc(sizeof(*request));
    if (!request)
        outofmem(sizeof(*request), "blueprint update request");
    *request = (program_update_request_t){0};
    for (i = 0; i < UPDATE_NUM_ROOTS; i++)
        put_number(&request->roots[i], 0);
    request->target_limit = BLUEPRINT_UPDATE_MAX_TARGETS;
    request->scan_limit = BLUEPRINT_UPDATE_MAX_SCAN_OBJECTS;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    {
        FILE *input = fopen("requests-limits", "r");
        unsigned long targets, scanned;
        if (input)
        {
            if (fscanf(input, "%lu %lu", &targets, &scanned) == 2
             && targets && targets <= request->target_limit
             && scanned && scanned <= request->scan_limit)
            {
                request->target_limit = targets;
                request->scan_limit = scanned;
            }
            fclose(input);
        }
    }
#endif
    request->budget = (schema_budget_t){ BLUEPRINT_UPDATE_MAX_BYTES,
                                         SCHEMA_MAX_WORK, BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS };
    request_failure(request, "PREPARATION_PENDING", "Blueprint preparation has not completed.");
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
    request_bytes(request, sizeof(*request) + request->source_program->total_size
                           + mstrsize(source->load_name) + 16384);
    if (request->explicit_selection)
    {
        vector_t *copy;
        struct pointer_table *identities;
        size_t capacity;
        selection = arg[1].u.vec;
        if (VEC_SIZE(selection) > request->scan_limit)
            errorf("update_blueprint(): explicit input scan limit exceeded.\n");
        capacity = VEC_SIZE(selection) < request->target_limit
                   ? VEC_SIZE(selection) : request->target_limit;
        request_bytes(request, 2 * (sizeof(vector_t) + capacity * sizeof(svalue_t))
                      + 4096 + (capacity < 256 ? capacity + 1 : 256) * 4096);
        put_array(&request->roots[UPDATE_TARGETS], allocate_array(capacity));
        copy = request->roots[UPDATE_TARGETS].u.vec;
        identities = push_new_pointer_table();
        if (!identities) outofmemory("blueprint target identity table");
        for (i = 0; i < VEC_SIZE(selection); i++)
        {
            object_t *target;
            if (selection->item[i].type != T_OBJECT)
                errorf("update_blueprint(): target must be an object.\n");
            target = selection->item[i].u.ob;
            if (lookup_pointer(identities, target))
                continue;
            request_bytes(request, 128); /* Pointer table record and bucket allowance. */
            find_add_pointer(identities, target, MY_TRUE);
            validate_object(target, source->load_name, MY_FALSE, MY_TRUE, request);
            if (!(target->flags & O_CLONE))
                continue;
            if (unique == request->target_limit)
                errorf("update_blueprint(): explicit clone target limit exceeded.\n");
            assign_svalue_no_free(&copy->item[unique++], &selection->item[i]);
        }
        pop_stack(); /* Temporary identity table; copied objects remain rooted. */
        selection = slice_array(copy, 0, unique - 1);
        free_svalue(&request->roots[UPDATE_TARGETS]);
        put_array(&request->roots[UPDATE_TARGETS], selection);
    }
    validate_capacity(request);
    if (request->explicit_selection) validate_targets(request, MY_TRUE);
    if (!privilege_violation2(STR_UPDATE_BLUEPRINT, arg,
                             num_arg == 2 ? &arg[1] : &const0, inter_sp))
        errorf("update_blueprint(): privilege denied.\n");
    if (request->canceled || !request->owner
     || request->roots[UPDATE_SOURCE].type != T_OBJECT)
        errorf("update_blueprint(): owner or source was destructed.\n");
    validate_source(request, MY_TRUE);
    validate_capacity(request);
    if (request->explicit_selection) validate_targets(request, MY_TRUE);
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

    report_test_step();
    push_c_string(inter_sp, name);
    result = REPORT_TEST_NULL(get_map_lvalue(report, inter_sp));
    if (!result)
        outofmem(sizeof(*result), "blueprint update report field");
    pop_stack();
    return result;
} /* report_field() */

typedef struct report_copy_s
{
    mapping_t *destination;
    struct pointer_table *pointers;
    schema_budget_t *budget;
    size_t *num_pointers;
    p_int width;
    unsigned int depth;
} report_copy_t;

/* Conservative allowances for the pointer table's pooled root/handler,
 * at most 256 second-level tables, and individual records/pool slack.
 */
#define REPORT_POINTER_TABLE_BYTES 4096
#define REPORT_POINTER_RECORD_BYTES 128
#define REPORT_POINTER_SUBTABLES 256
#define REPORT_METADATA_BYTES 16384

static void
report_copy_units(schema_budget_t *budget, size_t count, size_t unit)
/* Check before multiplying, including all report-only bookkeeping. */
{
    if (count > budget->bytes / unit)
        errorf("update_blueprint_result(): report copy size limit exceeded.\n");
    budget->bytes -= count * unit;
}

static void copy_report_value(svalue_t *destination, svalue_t *source,
                              report_copy_t *context);

static void
copy_report_row(svalue_t *key, svalue_t *values, void *extra)
{
    report_copy_t *context = extra;
    svalue_t *destination;
    /* A literal mapping may itself have an aggregate key. The key remains
     * stack-rooted throughout insertion and copying all of the row values.
     */
    push_number(inter_sp, 0);
    copy_report_value(inter_sp, key, context);
    destination = REPORT_TEST_NULL(get_map_lvalue(context->destination, inter_sp));
    if (!destination) outofmemory("blueprint report mapping row");
    for (p_int i = 0; i < context->width; i++)
        copy_report_value(&destination[i], &values[i], context);
    pop_stack();
}

static void
copy_report_value(svalue_t *destination, svalue_t *source, report_copy_t *context)
/* Reports contain only native scalar/array/mapping trees. Keep all temporary
 * containers rooted; a failed copy leaves the immutable request untouched.
 * Preserve shared aggregate identities, including literal mapping keys.
 */
{
    report_copy_t child = *context;
    struct pointer_record *record;
    assert_stack_gap();
    if (context->depth > BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH + 16
     || !context->budget->work-- || add_eval_cost(1))
        errorf("update_blueprint_result(): report copy work limit exceeded.\n");
    report_copy_units(context->budget, 1, sizeof(svalue_t));
    if (source->type == T_POINTER)
    {
        report_copy_units(context->budget, 1, sizeof(vector_t));
        report_copy_units(context->budget, VEC_SIZE(source->u.vec), sizeof(svalue_t));
    }
    else if (source->type == T_MAPPING)
    {
        size_t row = sizeof(svalue_t) + 6 * sizeof(void *);
        size_t width = source->u.map->num_values;
        if (width > (SIZE_MAX - row) / sizeof(svalue_t))
            errorf("update_blueprint_result(): report copy size overflow.\n");
        report_copy_units(context->budget, 1, sizeof(mapping_t) + sizeof(mapping_hash_t));
        report_copy_units(context->budget, MAP_SIZE(source->u.map), row + width * sizeof(svalue_t));
    }
    else if (source->type == T_STRING || source->type == T_BYTES)
    {
        report_copy_units(context->budget, 1, sizeof(string_t));
        report_copy_units(context->budget, mstrsize(source->u.str), 1);
    }
    child.depth++;
    switch (source->type)
    {
    case T_POINTER:
    case T_MAPPING:
        if (!lookup_pointer(context->pointers, source->u.generic))
        {
            report_copy_units(context->budget, 1, REPORT_POINTER_RECORD_BYTES);
            if ((*context->num_pointers)++ < REPORT_POINTER_SUBTABLES)
                report_copy_units(context->budget, 1, REPORT_POINTER_TABLE_BYTES);
        }
        report_test_step();
        record = find_add_pointer(context->pointers, source->u.generic, MY_TRUE);
        if (record->ref_count++ >= 0)
        {
            if (source->type == T_POINTER)
                put_ref_array(destination, record->data);
            else
                put_ref_mapping(destination, record->data);
            return;
        }
        if (source->type == T_POINTER)
        {
            report_test_step();
            put_array(destination, allocate_array(VEC_SIZE(source->u.vec)));
            record->data = destination->u.vec;
            for (size_t i = 0; i < VEC_SIZE(source->u.vec); i++)
                copy_report_value(&destination->u.vec->item[i], &source->u.vec->item[i], &child);
        }
        else
        {
            mapping_t *map;
            map = REPORT_TEST_NULL(allocate_mapping(MAP_SIZE(source->u.map), source->u.map->num_values));
            if (!map) outofmemory("blueprint report mapping");
            put_mapping(destination, map);
            record->data = map;
            child.destination = map;
            child.width = source->u.map->num_values;
            walk_mapping(source->u.map, copy_report_row, &child);
        }
        return;
    case T_NUMBER:
    case T_FLOAT:
    case T_STRING:
    case T_BYTES:
        assign_svalue_no_free(destination, source);
        return;
    default:
        fatal("Invalid native blueprint report value %d.\n", source->type);
    }
}

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
    size_t i, num_pointers = 0;
    schema_budget_t budget = { BLUEPRINT_UPDATE_MAX_BYTES,
                               BLUEPRINT_UPDATE_MAX_BYTES / sizeof(svalue_t), 0 };
    report_copy_t copy = { .budget = &budget, .num_pointers = &num_pointers };
    inter_sp = sp;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    if (active && active->committed)
        assert(!get_stack_gap_guard()); /* Includes the callback error hook. */
#endif
    expire_reports();
    for (request = requests; request; request = request->next)
        if (request->id && request->id == sp->u.number && !request->canceled)
            break;
    if (!request)
        errorf("update_blueprint_result(): unknown or expired request id.\n");
    if (get_current_object() != request->owner && get_current_object() != master_ob)
        errorf("update_blueprint_result(): request belongs to another object.\n");
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    {
        FILE *input = fopen("requests-poll-budget", "r");
        unsigned long bytes;
        if (input)
        {
            if (fscanf(input, "%lu", &bytes) == 1 && bytes < budget.bytes)
                budget.bytes = bytes;
            fclose(input);
        }
    }
#endif
    /* Covers the fixed root/reason mappings, keys and scalar fields. The
     * variable-length origin/message and outer arrays are charged separately.
     */
    report_copy_units(&budget, 1, REPORT_METADATA_BYTES);
    report_copy_units(&budget, mstrsize(request->roots[UPDATE_ORIGIN].u.str), 1);
    report_copy_units(&budget, sizeof(request->failure_message), 1);
    report_copy_units(&budget, 1, REPORT_POINTER_TABLE_BYTES);
    report_test_begin("requests-poll-fault");
    report = REPORT_TEST_NULL(allocate_mapping(15, 1));
    if (!report)
        outofmem(15, "blueprint update report");
    put_mapping(sp, report);
    put_number(report_field(report, "id"), request->id);
    put_ref_string(report_field(report, "origin"), request->roots[UPDATE_ORIGIN].u.str);
    put_c_string(report_field(report, "selection"), request->explicit_selection ? "explicit" : "all");
    put_c_string(report_field(report, "status"), request->committed ? "completed" : request->terminal ? "failed" : "pending");
    for (i = 0; i < sizeof(numeric_fields) / sizeof(numeric_fields[0]); i++)
        put_number(report_field(report, numeric_fields[i]), 0);
    put_number(report_field(report, "candidate_generation"), request->terminal ? request->candidate_generation : 0);
    put_number(report_field(report, "matched"), request->terminal ? request->matched : 0);
    put_number(report_field(report, "already_current"), request->terminal ? request->already_current : 0);
    put_number(report_field(report, "destroyed"), request->terminal ? request->destroyed : 0);
    put_number(report_field(report, "updated"), request->updated);
    put_number(report_field(report, "blueprint_updated"), request->blueprint_updated);
    put_number(report_field(report, "completed_at"), request->completed_at);
    copy.pointers = REPORT_TEST_NULL(push_new_pointer_table());
    if (!copy.pointers) outofmemory("blueprint report identity table");
    field = report_field(report, "variable_changes");
    if (request->terminal && request->roots[UPDATE_SCHEMAS].type == T_POINTER)
    {
        copy_report_value(field, &request->roots[UPDATE_SCHEMAS], &copy);
    }
    else
    {
        report_test_step();
        report_copy_units(&budget, 1, sizeof(vector_t));
        put_array(field, allocate_array(0));
    }
    field = report_field(report, "errors");
    report_test_step();
    report_copy_units(&budget, 1, sizeof(vector_t));
    if (request->terminal)
        report_copy_units(&budget, !request->committed + request->num_diagnostics + request->num_blockers, sizeof(svalue_t));
    put_array(field, allocate_array(request->terminal ? !request->committed + request->num_diagnostics + request->num_blockers : 0));
    if (request->terminal)
    {
        size_t offset = !request->committed;
        if (!request->committed)
        {
        error = REPORT_TEST_NULL(allocate_mapping(2, 1));
        if (!error)
            outofmem(2, "blueprint update failure report");
        put_mapping(&field->u.vec->item[0], error);
        put_c_string(report_field(error, "code"), request->failure_code);
        put_c_string(report_field(error, "message"), request->failure_message);
        }
        for (i = 0; i < request->num_diagnostics; i++)
            copy_report_value(&field->u.vec->item[i + offset],
                              &request->roots[UPDATE_DIAGNOSTICS].u.vec->item[i], &copy);
        if (request->roots[UPDATE_SCHEMAS].type == T_POINTER)
            for (size_t generation = 0; generation < VEC_SIZE(request->roots[UPDATE_SCHEMAS].u.vec); generation++)
            {
                mapping_t *schema = request->roots[UPDATE_SCHEMAS].u.vec->item[generation].u.map;
                for (int role = 0; role < (request->source_from_path && generation == 0 ? 2 : 1); role++)
                {
                    svalue_t *blocks;
                    push_c_string(inter_sp, role ? "blueprint_blockers" : "blockers");
                    blocks = get_map_value(schema, inter_sp);
                    pop_stack();
                    for (size_t j = 0; j < VEC_SIZE(blocks->u.vec); j++)
                        copy_report_value(&field->u.vec->item[offset + i++], &blocks->u.vec->item[j], &copy);
                }
            }
    }
    pop_stack(); /* Pointer table handler; report remains at sp. */
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    report_test_countdown = -1;
#endif
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

#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
static void
candidate_test_checkpoint (program_update_request_t *request, Bool collect)

/* Internal fault/GC proof, absent from normal driver builds. No LPC entry
 * point or scheduling change: run only after the parser and stack handlers
 * have relinquished all compiler scratch, with candidate on requests.
 */

{
    size_t i;
    size_t saved_array = max_array_size, saved_mapping = max_mapping_size;
    size_t saved_keys = max_mapping_keys;
    int32 saved_eval = max_eval_cost, saved_file = max_file_xfer;
    int32 saved_byte = max_byte_xfer, saved_callouts = max_callouts;
    int32 saved_use = use_eval_cost;
    p_int saved_memory = max_memory;
    int32 saved_cost = eval_cost, saved_assigned = assigned_eval_cost;
    int saved_privilege = malloc_privilege;

    if (strcmp(get_txt(request->roots[UPDATE_ORIGIN].u.str), "/staging_target"))
        return;
    for (i = 0; i < request->source_program->num_structs; i++)
    {
        struct_def_t *def = &request->source_program->struct_defs[i];
        if (def->inh == STRUCT_INH_LOCAL)
            assert(def->type->name->current == def->type);
    }
    if (collect)
    {
        assert(request->candidate);
        assert(!current_loc.file);
        mark_end_evaluation();
        clear_state();
        garbage_collection();
        max_array_size = saved_array;
        max_mapping_size = saved_mapping;
        max_mapping_keys = saved_keys;
        max_eval_cost = saved_eval;
        max_file_xfer = saved_file;
        max_byte_xfer = saved_byte;
        max_callouts = saved_callouts;
        max_memory = saved_memory;
        use_eval_cost = saved_use;
        eval_cost = saved_cost;
        assigned_eval_cost = saved_assigned;
        malloc_privilege = saved_privilege;
        mark_start_evaluation();
        set_current_object(request->owner);
        candidate_test_checkpoint(request, MY_FALSE);
        debug_message("BLUEPRINT_STAGING_NATIVE_GC: retained candidate and published types verified.\n");
    }
} /* candidate_test_checkpoint() */
#endif

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
 * Committed requests remain busy and reserve their family during retirement.
 * Native disposal cannot unwind its free queue; callback entry suspends the
 * native pressure guard only inside a complete Python-to-LPC error boundary.
 */

{
    struct error_recovery_info recovery;
    const size_t saved_array = max_array_size, saved_mapping = max_mapping_size;
    const size_t saved_keys = max_mapping_keys;
    const int32 saved_eval = max_eval_cost, saved_file = max_file_xfer;
    const int32 saved_byte = max_byte_xfer, saved_callouts = max_callouts;
    const int32 saved_use = use_eval_cost;
    const p_int saved_memory = max_memory;
    const int saved_privilege = malloc_privilege;
    /* A preceding player command can leave these borrowed identities set at
     * the periodic boundary. Candidate authorization belongs to its requester.
     */
    clear_state();
    eval_cost = assigned_eval_cost = 0;
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
        preparation_guard.failed = MY_FALSE;
        preparation_guard_active = MY_FALSE;
        previous_preparation_guard = get_stack_gap_guard();
        if (setjmp(recovery.con.text))
        {
            /* A native disposal queue cannot be recovered after an unwind.
             * Its guard and the Python callback boundary must contain errors.
             * Never turn an irreversible installation into a failed report.
             */
            if (active->committed)
                fatal("Unexpected unwind during blueprint retirement.\n");
            set_stack_gap_guard(&preparation_guard);
            preparation_guard_active = MY_TRUE;
            /* Release staged references before any live input/program roots.
             * Error-stack cleanup owns only native materialization scratch.
             */
            release_variables(active);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
            default_test_active = default_test_literal = MY_FALSE;
            report_test_countdown = -1;
#endif
            if (!strcmp(active->failure_code, "PREPARATION_PENDING")
             || !strcmp(active->failure_code, "SCHEMA_INCOMPATIBLE"))
                request_failure(active, "PREPARATION_FAILED", "Preparation failed before complete report evidence was available.");
            mark_end_evaluation();
            abort_compile_file_context();
            clear_state();
            /* Partial summaries are never exposed as completed evidence. */
            free_svalue(&active->roots[UPDATE_SCHEMAS]);
            put_number(&active->roots[UPDATE_SCHEMAS], 0);
            active->candidate_generation = 0;
            active->num_blockers = 0;
            if (preparation_guard_active)
            {
                set_stack_gap_guard(previous_preparation_guard);
                preparation_guard_active = MY_FALSE;
            }
        }
        else if (!active->canceled)
        {
            mark_start_evaluation();
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
            {
                FILE *input = fopen("requests-budget", "r");
                unsigned long bytes;
                if (input)
                {
                    if (fscanf(input, "%lu", &bytes) == 1 && bytes < active->budget.bytes)
                        active->budget.bytes = bytes;
                    fclose(input);
                }
            }
#endif
            if (!active->source_from_path) count_targets(active);
            /* Source-path family validation follows compilation/counting.
             * An earlier batch hook can queue replacement in this family;
             * rejecting it here would lose the surviving selection counts.
             */
            validate_source(active, !active->source_from_path);
            if (!active->source_from_path) validate_targets(active, MY_FALSE);
            /* Mapping allocations charge the submitting owner. This native
             * pass invokes no LPC and restores the idle backend context.
             */
            set_current_object(active->owner);
            if (active->source_from_path)
            {
                set_stack_gap_guard(&preparation_guard);
                preparation_guard_active = MY_TRUE;
                request_failure(active, "COMPILE_FAILED", "Candidate compilation failed.");
                compile_update_candidate(active->roots[UPDATE_ORIGIN].u.str,
                                         active->source_program, &active->candidate);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
                report_test_countdown = -1;
#endif
                set_stack_gap_guard(previous_preparation_guard);
                preparation_guard_active = MY_FALSE;
                /* This also covers pressure during the final context
                 * release, after the parser's last cancellation check.
                 */
                if (preparation_guard.failed)
                    errorf("update_blueprint(): memory pressure during compilation.\n");
                if (active->canceled || !active->owner
                 || (active->owner->flags & O_DESTRUCTED))
                    errorf("update_blueprint(): requester was destructed during compilation.\n");
                /* Freeze complete post-hook counts before source validation
                 * can reject a replacement queued anywhere in the family.
                 */
                count_targets(active);
                validate_source(active, MY_TRUE);
                validate_targets(active, MY_FALSE);
                if (!active->candidate)
                    errorf("update_blueprint(): candidate compilation failed.\n");
                request_bytes(active, active->candidate->total_size);
                request_failure(active, "PREPARATION_PENDING", "Blueprint preparation has not completed.");
            }
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
            if (active->candidate)
                candidate_test_checkpoint(active, MY_TRUE);
#endif
            set_stack_gap_guard(&preparation_guard);
            preparation_guard_active = MY_TRUE;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
            default_test_begin();
#endif
            describe_schemas(active);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
            default_test_active = default_test_literal = MY_FALSE;
#endif
            set_stack_gap_guard(previous_preparation_guard);
            preparation_guard_active = MY_FALSE;
            if (preparation_guard.failed)
                errorf("update_blueprint(): memory pressure preparing defaults.\n");
            if (!strcmp(active->failure_code, "PREPARATION_PENDING"))
                validate_runtime_dependencies(active);
            if (!strcmp(active->failure_code, "PREPARATION_PENDING"))
            {
                set_stack_gap_guard(&preparation_guard);
                preparation_guard_active = MY_TRUE;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
                migration_test_begin();
#endif
                push_error_handler(variables_cleanup, &active->handler);
                prepare_variables(active);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
                migration_test_checkpoint(active);
#endif
                prepare_publication(active);
                if (!strcmp(active->failure_code, "PREPARATION_PENDING"))
                    commit_variables(active);
                pop_stack(); /* Rollback unless ownership was committed. */
                set_stack_gap_guard(previous_preparation_guard);
                preparation_guard_active = MY_FALSE;
                if (preparation_guard.failed && !active->committed)
                    errorf("update_blueprint(): memory pressure preparing variables.\n");
            }
            clear_current_object();
            mark_end_evaluation();
        }
        if (active->candidate)
        {
            program_t *candidate = active->candidate;
            set_stack_gap_guard(&preparation_guard);
            preparation_guard_active = MY_TRUE;
            active->candidate = NULL;
            free_prog(candidate, MY_TRUE);
            if (preparation_guard.failed && !active->committed)
            {
                request_failure(active, "RESOURCE_FAILED", "Memory pressure while releasing the private candidate.");
                free_svalue(&active->roots[UPDATE_SCHEMAS]);
                put_number(&active->roots[UPDATE_SCHEMAS], 0);
                active->candidate_generation = 0;
                active->num_blockers = 0;
            }
            set_stack_gap_guard(previous_preparation_guard);
            preparation_guard_active = MY_FALSE;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
            if (!active->committed) candidate_test_checkpoint(active, MY_FALSE);
#endif
        }
        if (active->canceled && !active->committed)
            free_request(active);
        else
        {
            set_stack_gap_guard(&preparation_guard);
            release_inputs(active);
            clear_state();
            set_stack_gap_guard(previous_preparation_guard);
            active->busy = MY_FALSE;
            active->terminal = MY_TRUE;
            if (!active->committed) active->completed_at = report_time();
        }
        active = NULL;
        max_array_size = saved_array;
        max_mapping_size = saved_mapping;
        max_mapping_keys = saved_keys;
        max_eval_cost = saved_eval;
        max_file_xfer = saved_file;
        max_byte_xfer = saved_byte;
        max_callouts = saved_callouts;
        max_memory = saved_memory;
        use_eval_cost = saved_use;
        malloc_privilege = saved_privilege;
        eval_cost = assigned_eval_cost = 0;
    }
    rt_context = recovery.rt.last;
    expire_reports();
} /* program_update_process() */

void
program_update_owner_destructed (object_t *owner)

/* Cancel uncommitted requests owned by the destructing <owner> and remove
 * all non-owning owner links. Committed outcomes remain immutable. A busy
 * admission or backend request stays globally rooted with canceled set; its own error
 * handler or backend frame performs the eventual release. Other records
 * are synchronously freed. Call before <owner>'s storage can disappear.
 */

{
    while (owner->program_updates)
    {
        program_update_request_t *request = owner->program_updates;
        unlink_owner(request);
        if (request->committed)
            continue;
        if (request->busy || cleanup_depth)
            request->canceled = MY_TRUE;
        else
            free_request(request);
    }
} /* program_update_owner_destructed() */

void
program_update_shutdown (void)

/* Free every remaining request and report during idle backend shutdown.
 * No admission handler or active request evaluation may still own a record.
 * Uses the same rooted, guarded retirement contract as free_request().
 */

{
    while (requests)
        free_request(requests);
} /* program_update_shutdown() */

typedef struct update_cleanup_context_s
{
    error_handler_t handler;
    stack_gap_guard_t guard;
    stack_gap_guard_t *previous;
} update_cleanup_context_t;

static void
update_cleanup_end (error_handler_t *handler)
{
    update_cleanup_context_t *context = (update_cleanup_context_t *)handler;
    cleanup_depth--;
    set_stack_gap_guard(context->previous);
}

void
program_update_cleanup (cleanup_t *context)

/* Include request-owned aggregate roots in the ordinary data-clean pass,
 * which compacts mapping hashes before the collector clears references.
 * Invalid coroutine/closure cleanup can release arbitrary descendants. Keep
 * all existing request links stable through callbacks, and restore the guard
 * and traversal protection even if a data-clean allocation raises an error.
 */
{
    program_update_request_t *request;
    update_cleanup_context_t cleanup = {0};
    push_error_handler(update_cleanup_end, &cleanup.handler);
    cleanup.previous = set_stack_gap_guard(&cleanup.guard);
    cleanup_depth++;
    for (request = requests; request; request = request->next)
    {
        cleanup_vector(request->roots, UPDATE_NUM_ROOTS, context);
        for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
        {
            cleanup_vector(&stage->object, 1, context);
            if (stage->values) cleanup_vector(stage->values, stage->num_values, context);
        }
    }
    pop_stack();
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
        for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
        {
            clear_memory_reference(stage);
            clear_ref_in_vector(&stage->object, 1);
            if (stage->old_program) clear_program_ref(stage->old_program, MY_TRUE);
            if (stage->values)
            {
                clear_memory_reference(stage->values);
                clear_ref_in_vector(stage->values, stage->num_values);
            }
        }
        if (request->source_program)
            clear_program_ref(request->source_program, MY_TRUE);
        if (request->candidate)
            clear_program_ref(request->candidate, MY_TRUE);
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
        for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
        {
            note_malloced_block_ref(stage);
            count_ref_in_vector(&stage->object, 1);
            if (stage->old_program) mark_program_ref(stage->old_program);
            if (stage->values)
            {
                note_malloced_block_ref(stage->values);
                count_ref_in_vector(stage->values, stage->num_values);
            }
        }
        if (request->source_program)
            mark_program_ref(request->source_program);
        if (request->candidate)
            mark_program_ref(request->candidate);
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
        for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
        {
            count_extra_ref_in_vector(&stage->object, 1);
            if (stage->values) count_extra_ref_in_vector(stage->values, stage->num_values);
            if (stage->old_program)
            {
                stage->old_program->extra_ref++;
                count_extra_ref_in_prog(stage->old_program);
            }
        }
        if (request->source_program)
        {
            request->source_program->extra_ref++;
            count_extra_ref_in_prog(request->source_program);
        }
        if (request->candidate)
        {
            request->candidate->extra_ref++;
            count_extra_ref_in_prog(request->candidate);
        }
    }
} /* program_update_count_extra_refs() */
#endif /* DEBUG */
#endif /* USE_BLUEPRINT_UPDATE */
