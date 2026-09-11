#ifndef PROGRAM_UPDATE_H__
#define PROGRAM_UPDATE_H__ 1

#include "driver.h"
#include "typedefs.h"

#if defined(USE_BLUEPRINT_UPDATE) && defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
enum default_test_point
{
    DEFAULT_TEST_ARRAY = 1, DEFAULT_TEST_STRING, DEFAULT_TEST_MAPPING,
    DEFAULT_TEST_HASH, DEFAULT_TEST_CHAIN, DEFAULT_TEST_ARRAY_PRESSURE,
    DEFAULT_TEST_STRING_PRESSURE, DEFAULT_TEST_MAPPING_PRESSURE,
    DEFAULT_TEST_INSERT_PRESSURE, DEFAULT_TEST_DUPLICATE_RELEASE,
    DEFAULT_TEST_DUPLICATE_CHILD, DEFAULT_TEST_ROW_CHILD,
    DEFAULT_TEST_REPORT_MAPPING, DEFAULT_TEST_REPORT_STRING,
    DEFAULT_TEST_REPORT_HASH, DEFAULT_TEST_REPORT_CHAIN, DEFAULT_TEST_REPORT_ARRAY
};
extern Bool program_update_default_test_fail(int point);
extern void program_update_default_test_scope(Bool literal);
extern void program_update_default_test_pressure(int point);
extern void program_update_migration_test_step(void);
extern Bool program_update_binding_test_fail(void);
#define BINDING_TEST_FAIL() program_update_binding_test_fail()
#define MIGRATION_TEST_STEP() program_update_migration_test_step()
#define DEFAULT_TEST_NULL(point, allocation) \
    (program_update_default_test_fail(point) ? NULL : (allocation))
#define DEFAULT_TEST_PRESSURE(point) program_update_default_test_pressure(point)
#else
#define BINDING_TEST_FAIL() MY_FALSE
#define MIGRATION_TEST_STEP() ((void)0)
#define DEFAULT_TEST_NULL(point, allocation) (allocation)
#define DEFAULT_TEST_PRESSURE(point) ((void)0)
#endif

#ifdef USE_BLUEPRINT_UPDATE

enum program_dependency_kind
{
    PROGRAM_DEPENDENCY_BINDING,
    PROGRAM_DEPENDENCY_LFUN,
    PROGRAM_DEPENDENCY_COROUTINE
};

/* Weak intrusive membership, embedded in its native handle. Neither the
 * object head nor these links own/mark a reference. A closure's two records
 * are peers so destruction of either endpoint invalidates both records.
 */
typedef struct program_dependency_s
{
    object_t *object;
    struct program_dependency_s *next;
    struct program_dependency_s **prev;
    struct program_dependency_s *peer;
    void *handle;
    enum program_dependency_kind kind;
} program_dependency_t;

extern void program_dependency_init(program_dependency_t *link, void *handle,
                                    enum program_dependency_kind kind);
extern void program_dependency_attach(program_dependency_t *link, object_t *ob);
extern void program_dependency_detach(program_dependency_t *link);
extern void program_dependencies_clear(object_t *ob);

#define BLUEPRINT_UPDATE_MAX_DIAGNOSTICS 128
#define BLUEPRINT_UPDATE_MAX_DIAGNOSTIC_BYTES 65536

extern void program_update_compile_failure(const char *code, const char *message);
extern void program_update_compile_diagnostic(string_t *file, int line, Bool warning,
                                              string_t *message);

/* --- LPC request and report interface --- */

extern svalue_t *v_update_blueprint(svalue_t *sp, int num_arg);
extern svalue_t *f_update_blueprint_result(svalue_t *sp);

/* --- Backend lifecycle ---
 * Detach at periodic-tick entry. Process only with an idle VM after time,
 * flags, and the callout cycle have advanced, before heartbeat/callouts.
 * Both queue chains remain owned by the module's global GC root list.
 */

extern void program_update_detach(void);
extern void program_update_process(void);
extern Bool program_update_compilation_valid(void);

/* Destruction removes non-owning owner links and cancels uncommitted work.
 * Shutdown requires an idle backend with no remaining admission/evaluation.
 * Callback-capable retirement keeps the request rooted and its family busy;
 * committed outcomes survive owner destruction and retirement callbacks.
 */

extern void program_update_owner_destructed(object_t *owner);
extern void program_update_shutdown(void);
extern void program_update_cleanup(cleanup_t *context);

/* --- GC and DEBUG root accounting --- */

#ifdef GC_SUPPORT
extern void program_update_clear_refs(void);
extern void program_update_count_refs(void);
#endif /* GC_SUPPORT */

#ifdef DEBUG
extern void program_update_count_extra_refs(void);
#endif /* DEBUG */

#endif /* USE_BLUEPRINT_UPDATE */
#endif /* PROGRAM_UPDATE_H__ */
