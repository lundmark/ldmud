#ifndef PROGRAM_UPDATE_H__
#define PROGRAM_UPDATE_H__ 1
#include "driver.h"
#include "typedefs.h"
#ifdef USE_BLUEPRINT_UPDATE
extern svalue_t *v_update_blueprint(svalue_t *sp, int num_arg);
extern svalue_t *f_update_blueprint_result(svalue_t *sp);
extern void program_update_detach(void);
extern void program_update_process(void);
extern void program_update_owner_destructed(object_t *owner);
extern void program_update_shutdown(void);
#ifdef GC_SUPPORT
extern void program_update_clear_refs(void);
extern void program_update_count_refs(void);
#endif
#ifdef DEBUG
extern void program_update_count_extra_refs(void);
#endif
#endif
#endif /* PROGRAM_UPDATE_H__ */
