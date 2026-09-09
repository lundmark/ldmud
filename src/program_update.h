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
#define DEFAULT_TEST_NULL(point, allocation) \
    (program_update_default_test_fail(point) ? NULL : (allocation))
#define DEFAULT_TEST_PRESSURE(point) program_update_default_test_pressure(point)
#else
#define DEFAULT_TEST_NULL(point, allocation) (allocation)
#define DEFAULT_TEST_PRESSURE(point) ((void)0)
#endif

#ifdef USE_BLUEPRINT_UPDATE

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

/* Destruction removes non-owning owner links and marks active work canceled.
 * Shutdown requires an idle backend with no remaining admission/evaluation.
 * Current native teardown is non-reentrant; see the source contracts before
 * adding compiler cleanup or native release hooks that can reenter the VM.
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
