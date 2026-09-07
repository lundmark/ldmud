#ifndef PROGRAM_SCHEMA_H__
#define PROGRAM_SCHEMA_H__ 1

#include "driver.h"
#include "typedefs.h"

#ifdef USE_BLUEPRINT_UPDATE
/* Both programs must be loaded and pinned. Result must be a rooted zero
 * svalue; remaining is the shared conservative report memory budget.
 * Builds only scalar/array/mapping summaries, never changes programs
 * or objects. Allocation errors unwind through the caller's recovery frame.
 */
extern void program_schema_compare(program_t *old, program_t *candidate,
                                   svalue_t *result, size_t *remaining);
#ifdef DEBUG
extern void program_schema_check(const program_t *prog);
#endif
#endif
#endif /* PROGRAM_SCHEMA_H__ */
