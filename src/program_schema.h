#ifndef PROGRAM_SCHEMA_H__
#define PROGRAM_SCHEMA_H__ 1

#include "driver.h"
#include "typedefs.h"

#ifdef USE_BLUEPRINT_UPDATE
#define SCHEMA_MAX_WORK 1000000

typedef struct schema_budget_s
{
    size_t bytes;
    size_t work;
    size_t slots;
} schema_budget_t;

/* Both programs must be loaded and pinned. Result must be a rooted zero
 * svalue; budget holds the shared memory, work, and slot allowances.
 * Builds only scalar/array/mapping summaries, never changes programs
 * or objects. During source preparation, returns whether all required
 * defaults were prepared; otherwise returns full schema compatibility.
 * Allocation errors unwind through the caller's recovery frame.
 */
extern Bool program_schema_compare(program_t *old, program_t *candidate,
                                   svalue_t *result, schema_budget_t *budget,
                                   vector_t *prepared, Bool preparing_blueprint);
/* Full source-only comparison reuses descriptions from the rooted prepass
 * for this exact program pair; it never rematerializes literal defaults.
 */
extern Bool program_schema_compare_blueprint(program_t *old, program_t *candidate,
                                             svalue_t *result, schema_budget_t *budget,
                                             svalue_t *preparation);
/* Populate an already rooted candidate-sized invalid block. The validated
 * report supplies declaration matches; blueprint_values addresses either
 * the complete prepared source block or the pinned loaded blueprint block.
 * All old/source values remain untouched and independently owned.
 */
extern void program_schema_prepare_variables(program_t *old, program_t *candidate,
                                             mapping_t *report, svalue_t *old_values,
                                             svalue_t *values, svalue_t *blueprint_values,
                                             Bool blueprint, schema_budget_t *budget);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
extern void program_schema_test_defaults(program_t *source, svalue_t *root);
extern void program_schema_test_rtt(program_t *source);
#endif
#ifdef DEBUG
extern void program_schema_check(const program_t *prog);
#endif
#endif
#endif /* PROGRAM_SCHEMA_H__ */
