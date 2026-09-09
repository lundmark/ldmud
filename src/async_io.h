#ifndef ASYNC_IO_H__
#define ASYNC_IO_H__ 1

#include "driver.h"
#include "typedefs.h"

#ifdef USE_ASYNC_IO
#include <sys/select.h>
#include <stddef.h>

typedef struct async_request_s async_request_t;

/* begin() pushes an error handler on inter_sp. publish() consumes that
 * handler; all preparation above it must have been popped first. */
extern async_request_t *async_io_begin(svalue_t *callback);
extern void async_io_validate(async_request_t *request);
extern void async_io_set_path(async_request_t *request, const char *path);
extern void async_io_append(async_request_t *request, const void *data, size_t size);
extern void async_io_publish(async_request_t *request, unsigned int operation);

extern Bool async_io_start(const char *helper);
extern void async_io_cleanup(void);
extern Bool async_io_shutdown(void);
extern void async_io_set_fds(fd_set *readfds, fd_set *writefds, int *nfds);
extern void async_io_process_fds(fd_set *readfds, fd_set *writefds);
extern Bool async_io_pending(void);
extern void async_io_dispatch(void);
extern void async_io_remove_stale(void);
#ifdef DEBUG
extern void async_io_count_extra_refs(void);
#endif
#ifdef GC_SUPPORT
extern void async_io_clear_refs(void);
extern void async_io_count_refs(void);
#endif

#endif /* USE_ASYNC_IO */
#endif /* ASYNC_IO_H__ */
