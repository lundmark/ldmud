#ifndef PROGRAM_UPDATE_H__
#define PROGRAM_UPDATE_H__ 1

#include "driver.h"
#include "typedefs.h"

#ifdef USE_BLUEPRINT_UPDATE

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
