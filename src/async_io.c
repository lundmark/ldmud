/* Driver-owned asynchronous file requests. The companion executable alone
 * performs filesystem I/O; no LPC pointers cross the private socket. */
#include "driver.h"
#include "async_io.h"

#ifdef USE_ASYNC_IO
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "async_io_protocol.h"
#include "actions.h"
#include "backend.h"
#include "comm.h"
#include "gcollect.h"
#include "interpret.h"
#include "main.h"
#include "object.h"
#include "simulate.h"
#include "svalue.h"
#include "xalloc.h"
#include "i-current_object.h"
#include "i-eval_cost.h"

#define ASYNC_IO_TURN_BYTES 65536
#define ASYNC_IO_TURN_CALLBACKS 64
#define ASYNC_IO_START_SECONDS 5

enum async_state
{
    ASYNC_CONSTRUCTING, ASYNC_QUEUED, ASYNC_SENDING,
    ASYNC_AWAITING, ASYNC_COMPLETE, ASYNC_RETIRED
};

struct async_request_s
{
    error_handler_t error_handler;
    async_request_t *root_next;
    async_request_t *queue_next;
    callback_t callback;
    object_t *giver;
    enum async_state state;
    uint64_t id;
    unsigned char header[AIO_REQUEST_SIZE];
    unsigned char *payload;
    size_t length, capacity, sent, path_length;
    char path[AIO_MAX_PATH + 1];
    char temporary[AIO_MAX_PATH + 1];
    uint64_t device, inode;
    int result;
};

static async_request_t *roots, *queue_head, *queue_tail, *active;
static size_t request_count, payload_capacity;
static uint64_t next_id;
static int helper_fd = -1;
static pid_t helper_pid = -1;
static Bool accepting, hello_received;
static Bool drain_failed;
static Bool cleanup_waited;
static double lost_deadline;
static unsigned char reply[AIO_REPLY_SIZE + AIO_MAX_PATH];
static size_t reply_used, reply_needed = AIO_REPLY_SIZE;

/*-------------------------------------------------------------------------*/
static double
monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (double)now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void
cleanup_temporary(async_request_t *request)
{
    struct stat st;
    /* Only remove a temporary whose creation and identity were acknowledged.
     * Never infer a name after a lost progress record. */
    if (request->temporary[0]
     && lstat(request->temporary, &st) == 0
     && (uint64_t)st.st_dev == request->device
     && (uint64_t)st.st_ino == request->inode)
        unlink(request->temporary);
    request->temporary[0] = '\0';
}

static void
free_request(async_request_t *request)
{
    async_request_t **link;
    for (link = &roots; *link != request; link = &(*link)->root_next)
        NOOP;
    *link = request->root_next;
    free_callback(&request->callback);
    if (request->giver)
        free_object(request->giver, "async file request");
    if (request->payload)
        xfree(request->payload);
    payload_capacity -= request->capacity;
    request_count--;
    xfree(request);
}

static void
construction_error(error_handler_t *handler)
{
    async_request_t *request = (async_request_t *)handler;
    if (request->state == ASYNC_CONSTRUCTING)
        free_request(request);
}

async_request_t *
async_io_begin(svalue_t *callback)
{
    async_request_t *request;
    int callback_error;
    if (!accepting || helper_fd < 0)
        errorf("Asynchronous file writer unavailable or shutting down.\n");
    if (request_count >= ASYNC_IO_MAX_REQUESTS)
        errorf("Too many outstanding asynchronous file requests.\n");
    request = xalloc(sizeof(*request));
    if (!request)
        errorf("Out of memory for asynchronous file request.\n");
    memset(request, 0, sizeof(*request));
    init_empty_callback(&request->callback);
    request->root_next = roots;
    roots = request;
    request_count++;
    push_error_handler(construction_error, &request->error_handler);
    /* Adopt the argument only after its new owner is error-protected. */
    callback_error = setup_closure_callback(&request->callback, callback, 0, NULL, true);
    /* Transfer helpers adopt without clearing the source slot. Our efuns
     * later pop their arguments normally, unlike call_out(), so invalidate
     * that slot explicitly on both success and rejection. */
    callback->type = T_INVALID;
    if (callback_error >= 0)
        errorf("Invalid asynchronous file callback.\n");
    if (command_giver)
        request->giver = ref_object(command_giver, "async file request");
    return request;
}

void
async_io_set_path(async_request_t *request, const char *path)
{
    size_t length = strlen(path);
    if (!length || length > AIO_MAX_PATH)
        errorf("Invalid asynchronous file path length.\n");
    memcpy(request->path, path, length + 1);
    request->path_length = length;
}

void
async_io_append(async_request_t *request, const void *data, size_t size)
{
    size_t required, capacity, available;
    unsigned char *buffer;
    if (size > ASYNC_IO_MAX_REQUEST_BYTES - request->length)
        errorf("Asynchronous file request exceeds its payload limit.\n");
    required = request->length + size;
    if (required > request->capacity)
    {
        available = ASYNC_IO_MAX_BYTES - payload_capacity;
        if (required - request->capacity > available)
            errorf("Outstanding asynchronous file payload limit exceeded.\n");
        capacity = request->capacity ? request->capacity : 4096;
        while (capacity < required)
            capacity *= 2;
        if (capacity > ASYNC_IO_MAX_REQUEST_BYTES)
            capacity = ASYNC_IO_MAX_REQUEST_BYTES;
        if (capacity - request->capacity > available)
            capacity = request->capacity + available;
        buffer = request->payload ? rexalloc(request->payload, capacity)
                                  : xalloc(capacity);
        if (!buffer)
            errorf("Out of memory for asynchronous file payload.\n");
        payload_capacity += capacity - request->capacity;
        request->capacity = capacity;
        request->payload = buffer;
    }
    if (size)
        memcpy(request->payload + request->length, data, size);
    request->length = required;
}

void
async_io_validate(async_request_t *request)
{
    if (!accepting || helper_fd < 0)
        errorf("Asynchronous file writer unavailable or shutting down.\n");
    if (is_current_object_destructed()
     || !valid_callback_object(&request->callback))
        errorf("Asynchronous file caller or callback was destructed.\n");
}

void
async_io_publish(async_request_t *request, unsigned int operation)
{
    async_io_validate(request);
    if (!request->path_length || next_id == UINT64_MAX)
        errorf("Cannot publish asynchronous file request.\n");
    request->id = ++next_id;
    aio_put_u32(request->header, AIO_MAGIC);
    aio_put_u32(request->header + 4, AIO_VERSION);
    aio_put_u32(request->header + 8, operation);
    aio_put_u32(request->header + 12, request->path_length);
    aio_put_u64(request->header + 16, request->length);
    aio_put_u64(request->header + 24, request->id);
    request->state = ASYNC_QUEUED;
    if (queue_tail)
        queue_tail->queue_next = request;
    else
        queue_head = request;
    queue_tail = request;
    free_svalue(inter_sp--); /* Dismiss the construction owner. */
    comm_return_to_backend = MY_TRUE;
}

/*-------------------------------------------------------------------------*/
static void
writer_lost(const char *reason, int error)
{
    async_request_t *request;
    debug_message("%s Async file writer lost: %s (errno %d); "
                  "in-flight file outcome may be uncertain.\n",
                  time_stamp(), reason, error);
    accepting = MY_FALSE;
    if (helper_fd >= 0)
        close(helper_fd);
    helper_fd = -1;
    if (helper_pid > 0)
    {
        kill(helper_pid, SIGKILL);
        lost_deadline = monotonic_seconds() + 0.5;
    }
    for (request = queue_head; request; request = request->queue_next)
        if (request->state != ASYNC_COMPLETE)
        {
            request->result = -1;
            request->state = ASYNC_COMPLETE;
        }
    active = NULL;
    comm_return_to_backend = MY_TRUE;
}

static Bool
consume_reply(void)
{
    unsigned int kind = aio_get_u32(reply + 8);
    unsigned int error = aio_get_u32(reply + 12);
    uint64_t id = aio_get_u64(reply + 16);
    unsigned int path_length = aio_get_u32(reply + 24);
    unsigned int result = aio_get_u32(reply + 28);
    if (aio_get_u32(reply) != AIO_MAGIC
     || aio_get_u32(reply + 4) != AIO_VERSION
     || aio_get_u32(reply + 52) != 0)
        return MY_FALSE;
    if (!hello_received)
    {
        if (kind != AIO_REPLY_HELLO || id || path_length || result || error
         || aio_get_u64(reply + 32) || aio_get_u64(reply + 40)
         || aio_get_u32(reply + 48))
            return MY_FALSE;
        hello_received = MY_TRUE;
        return MY_TRUE;
    }
    if (!active || active->state != ASYNC_AWAITING || id != active->id)
        return MY_FALSE;
    if (kind == AIO_REPLY_TEMP)
    {
        const char *slash = strrchr(active->path, '/');
        size_t directory_length = slash ? (size_t)(slash - active->path) + 1 : 0;
        if (!path_length || path_length > AIO_MAX_PATH || result || error
         || aio_get_u32(active->header + 8) != AIO_OP_SAVE
         || active->temporary[0]
         || memchr(reply + AIO_REPLY_SIZE, '\0', path_length))
            return MY_FALSE;
        if (path_length != directory_length + sizeof(".ldmud-async-XXXXXX") - 1
         || memcmp(reply + AIO_REPLY_SIZE, active->path, directory_length)
         || memcmp(reply + AIO_REPLY_SIZE + directory_length, ".ldmud-async-", 13)
         || memchr(reply + AIO_REPLY_SIZE + directory_length, '/',
                   path_length - directory_length))
            return MY_FALSE;
        memcpy(active->temporary, reply + AIO_REPLY_SIZE, path_length);
        active->temporary[path_length] = '\0';
        active->device = aio_get_u64(reply + 32);
        active->inode = aio_get_u64(reply + 40);
        return MY_TRUE;
    }
    if (kind != AIO_REPLY_RESULT || path_length || result > 1
     || aio_get_u64(reply + 32) || aio_get_u64(reply + 40)
     || (!result && error))
        return MY_FALSE;
    if (!result && aio_get_u32(active->header + 8) == AIO_OP_SAVE
     && !active->temporary[0])
        return MY_FALSE;
    if (result)
        debug_message("%s Async file request %llu failed at stage %u "
                      "(errno %u).\n", time_stamp(),
                      (unsigned long long)id, aio_get_u32(reply + 48), error);
    active->result = result ? -1 : 0;
    active->state = ASYNC_COMPLETE;
    /* A normal result means the helper has completed its own cleanup. */
    active->temporary[0] = '\0';
    active = NULL;
    comm_return_to_backend = MY_TRUE;
    return MY_TRUE;
}

void
async_io_set_fds(fd_set *readfds, fd_set *writefds, int *nfds)
{
    async_request_t *request;
    if (helper_fd < 0)
        return;
    if (!active && hello_received)
        for (request = queue_head; request; request = request->queue_next)
            if (request->state == ASYNC_QUEUED)
            {
                active = request;
                active->state = ASYNC_SENDING;
                break;
            }
    FD_SET(helper_fd, readfds);
    if (active && active->state == ASYNC_SENDING)
        FD_SET(helper_fd, writefds);
    if (helper_fd >= *nfds)
        *nfds = helper_fd + 1;
}

void
async_io_process_fds(fd_set *readfds, fd_set *writefds)
{
    size_t budget = ASYNC_IO_TURN_BYTES;
    int retries = 8;
    if (helper_fd < 0)
        return;
    while (active && active->state == ASYNC_SENDING && budget
        && FD_ISSET(helper_fd, writefds))
    {
        const unsigned char *data;
        size_t offset = active->sent, length;
        ssize_t sent;
        if (offset < AIO_REQUEST_SIZE)
        {
            data = active->header + offset;
            length = AIO_REQUEST_SIZE - offset;
        }
        else if (offset - AIO_REQUEST_SIZE < active->path_length)
        {
            offset -= AIO_REQUEST_SIZE;
            data = (unsigned char *)active->path + offset;
            length = active->path_length - offset;
        }
        else
        {
            offset -= AIO_REQUEST_SIZE + active->path_length;
            length = active->length - offset;
            if (!length)
            {
                active->state = ASYNC_AWAITING;
                break;
            }
            data = active->payload + offset;
        }
        if (length > budget)
            length = budget;
        sent = send(helper_fd, data, length,
#ifdef MSG_NOSIGNAL
                    MSG_NOSIGNAL
#else
                    0
#endif
                   );
        if (sent < 0)
        {
            if (errno == EINTR && --retries > 0)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            writer_lost("send", errno);
            return;
        }
        if (!sent)
        {
            writer_lost("zero-length send", EIO);
            return;
        }
        active->sent += sent;
        budget -= sent;
        comm_return_to_backend = MY_TRUE;
        if (active->sent == AIO_REQUEST_SIZE + active->path_length + active->length)
            active->state = ASYNC_AWAITING;
    }
    while (helper_fd >= 0 && budget && FD_ISSET(helper_fd, readfds))
    {
        size_t length = reply_needed - reply_used;
        ssize_t received;
        if (length > budget)
            length = budget;
        received = recv(helper_fd, reply + reply_used, length, 0);
        if (received < 0)
        {
            if (errno == EINTR && --retries > 0)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            writer_lost("receive", errno);
            return;
        }
        if (!received)
        {
            writer_lost("EOF", 0);
            return;
        }
        reply_used += received;
        budget -= received;
        comm_return_to_backend = MY_TRUE;
        if (reply_used == AIO_REPLY_SIZE)
        {
            uint32_t path_length = aio_get_u32(reply + 24);
            if (path_length > AIO_MAX_PATH
             || (path_length && aio_get_u32(reply + 8) != AIO_REPLY_TEMP))
            {
                writer_lost("invalid reply length", EPROTO);
                return;
            }
            reply_needed = AIO_REPLY_SIZE + path_length;
        }
        if (reply_used == reply_needed)
        {
            if (!consume_reply())
            {
                writer_lost("invalid reply", EPROTO);
                return;
            }
            reply_used = 0;
            reply_needed = AIO_REPLY_SIZE;
        }
    }
}

Bool
async_io_pending(void)
{
    return queue_head && queue_head->state == ASYNC_COMPLETE;
}

static void
dispatch_one(async_request_t *request)
{
    struct error_recovery_info recovery;
    recovery.rt.last = rt_context;
    recovery.rt.type = ERROR_RECOVERY_BACKEND;
    rt_context = &recovery.rt;
    current_interactive = NULL;
    if (setjmp(recovery.con.text))
    {
        mark_end_evaluation();
        clear_state();
        debug_message("%s Error in async file callback.\n", time_stamp());
    }
    else
    {
        RESET_LIMITS;
        CLEAR_EVAL_COST;
        tracedepth = 0;
        trace_level = 0;
        command_giver = request->giver;
        if (command_giver && (command_giver->flags & O_DESTRUCTED))
            command_giver = NULL;
        mark_start_evaluation();
        push_number(inter_sp, request->result);
        (void)backend_callback(&request->callback, 1);
        mark_end_evaluation();
        clear_state();
    }
    rt_context = recovery.rt.last;
}

void
async_io_dispatch(void)
{
    unsigned int count = 0;
    async_request_t *request, *batch, **last = &batch;
    if (helper_fd < 0 && helper_pid > 0)
    {
        int status;
        pid_t result = waitpid(helper_pid, &status, WNOHANG);
        if (result == helper_pid || (result < 0 && errno == ECHILD))
            helper_pid = -1;
        else if (monotonic_seconds() < lost_deadline)
        {
            /* Do not block the game loop while waiting for termination. */
            comm_return_to_backend = MY_TRUE;
            return;
        }
    }
    batch = NULL;
    /* Detach a bounded prefix. The root list owns it throughout callbacks,
     * including errors or reentrant submissions. */
    while (async_io_pending() && count++ < ASYNC_IO_TURN_CALLBACKS)
    {
        request = queue_head;
        queue_head = request->queue_next;
        *last = request;
        last = &request->queue_next;
    }
    *last = NULL;
    if (!queue_head)
        queue_tail = NULL;
    while (batch)
    {
        request = batch;
        batch = request->queue_next;
        if (!accepting && request->result)
            drain_failed = MY_TRUE;
        dispatch_one(request);
        /* A dead helper must be stopped before removing its known temporary:
         * otherwise it could still be writing or about to rename that file. */
        if (helper_fd < 0 && helper_pid <= 0)
            cleanup_temporary(request);
        if (helper_pid > 0 && request->temporary[0])
        {
            /* A child in uninterruptible I/O may outlive SIGKILL. Preserve
             * its acknowledged identity until reap, independently of the
             * delivered callback and the much larger payload. */
            free_callback(&request->callback);
            init_empty_callback(&request->callback);
            if (request->giver)
                free_object(request->giver, "async retired command giver");
            request->giver = NULL;
            if (request->payload)
                xfree(request->payload);
            request->payload = NULL;
            payload_capacity -= request->capacity;
            request->capacity = request->length = 0;
            request->state = ASYNC_RETIRED;
        }
        else
            free_request(request);
    }
    if (async_io_pending())
        comm_return_to_backend = MY_TRUE;
}

/*-------------------------------------------------------------------------*/
static void
stop_child(void)
{
    int phase;
    if (helper_pid <= 0)
        return;
    if (cleanup_waited)
    {
        int status;
        pid_t result = waitpid(helper_pid, &status, WNOHANG);
        if (result == helper_pid || (result < 0 && errno == ECHILD))
            helper_pid = -1;
        return;
    }
    cleanup_waited = MY_TRUE;
    for (phase = 0; phase < 2; phase++)
    {
        int attempt;
        kill(helper_pid, phase ? SIGKILL : SIGTERM);
        for (attempt = 0; attempt < 25; attempt++)
        {
            int status;
            pid_t result = waitpid(helper_pid, &status, WNOHANG);
            struct timeval timeout = { 0, 10000 };
            if (result == helper_pid || (result < 0 && errno == ECHILD))
            {
                helper_pid = -1;
                return;
            }
            select(0, NULL, NULL, NULL, &timeout);
        }
    }
    debug_message("%s Async file helper %ld did not reap within 500 ms.\n",
                  time_stamp(), (long)helper_pid);
}

void
async_io_cleanup(void)
{
    accepting = MY_FALSE;
    if (helper_fd >= 0)
        close(helper_fd);
    helper_fd = -1;
    stop_child();
    if (roots)
        debug_message("%s Discarding %zu async file requests during cleanup; "
                      "no LPC callbacks will run.\n", time_stamp(), request_count);
    while (roots)
    {
        if (helper_pid <= 0)
            cleanup_temporary(roots);
        free_request(roots);
    }
    queue_head = queue_tail = active = NULL;
}

static void
pump(double seconds)
{
    fd_set readfds, writefds;
    struct timeval timeout;
    int nfds = 0, result;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    async_io_set_fds(&readfds, &writefds, &nfds);
    if (seconds < 0)
        seconds = 0;
    timeout.tv_sec = (long)seconds;
    timeout.tv_usec = (long)((seconds - timeout.tv_sec) * 1000000);
    result = select(nfds, &readfds, &writefds, NULL, &timeout);
    if (result > 0)
        async_io_process_fds(&readfds, &writefds);
    else if (result < 0 && errno != EINTR)
        writer_lost("select", errno);
}

Bool
async_io_start(const char *helper)
{
    int sockets[2], flags, error_fd;
    long close_limit;
    double deadline, now;
    char * const argv[] = { (char *)helper, NULL };
    if (!helper || helper[0] != '/' || access(helper, X_OK) < 0)
    {
        fprintf(stderr, "Async file helper must be an executable absolute path: %s\n",
                helper ? helper : "(unset)");
        return MY_FALSE;
    }
    now = monotonic_seconds();
    close_limit = sysconf(_SC_OPEN_MAX);
    if (now < 0 || close_limit < 0 || close_limit > INT_MAX)
        return MY_FALSE;
    /* Capture stderr before socketpair can reuse a closed standard fd. The
     * saved descriptor must survive all three dup2 operations in the child. */
    error_fd = fcntl(STDERR_FILENO, F_DUPFD, STDERR_FILENO + 1);
    if (error_fd < 0)
    {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0)
            return MY_FALSE;
        error_fd = fcntl(null_fd, F_DUPFD, STDERR_FILENO + 1);
        close(null_fd);
    }
    if (error_fd < 0)
        return MY_FALSE;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
        close(error_fd);
        return MY_FALSE;
    }
    if (sockets[0] >= FD_SETSIZE)
    {
        close(sockets[0]);
        close(sockets[1]);
        close(error_fd);
        return MY_FALSE;
    }
    {
        DIR *directory = opendir("/proc/self/fd");
        struct dirent *entry;
        Bool listed = MY_TRUE;
        if (!directory)
            directory = opendir("/dev/fd");
        if (!directory)
            listed = MY_FALSE;
        else
        {
            /* Existing descriptors can exceed a subsequently lowered soft
             * limit. Inventory them before fork. The close loop also covers
             * the current limit, including descriptors opened by package
             * threads between this inventory and fork. */
            for (;;)
            {
                char *end;
                long descriptor;
                errno = 0;
                entry = readdir(directory);
                if (!entry)
                {
                    listed = errno == 0;
                    break;
                }
                if (entry->d_name[0] == '.')
                    continue;
                descriptor = strtol(entry->d_name, &end, 10);
                if (*end || descriptor < 0 || descriptor >= INT_MAX || errno)
                {
                    listed = MY_FALSE;
                    break;
                }
                if (descriptor >= close_limit)
                    close_limit = descriptor + 1;
            }
            closedir(directory);
        }
        if (!listed)
        {
            fprintf(stderr, "Async file writer requires an accessible /proc/self/fd or /dev/fd.\n");
            close(error_fd);
            close(sockets[0]);
            close(sockets[1]);
            return MY_FALSE;
        }
    }
    helper_pid = fork();
    if (helper_pid == 0)
    {
        int fd;
        /* No allocation, stdio, locks or interpreter code after fork. */
        if (dup2(sockets[1], STDIN_FILENO) < 0
         || dup2(STDIN_FILENO, STDOUT_FILENO) < 0
         || dup2(error_fd, STDERR_FILENO) < 0)
            _exit(127);
        for (fd = STDERR_FILENO + 1; fd < close_limit; fd++)
            close(fd);
        execv(helper, argv);
        _exit(127);
    }
    close(error_fd);
    close(sockets[1]);
    if (helper_pid < 0)
    {
        close(sockets[0]);
        return MY_FALSE;
    }
    helper_fd = sockets[0];
    flags = fcntl(helper_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(helper_fd, F_SETFL, flags | O_NONBLOCK) < 0
     || fcntl(helper_fd, F_SETFD, FD_CLOEXEC) < 0)
    {
        async_io_cleanup();
        return MY_FALSE;
    }
#ifdef SO_NOSIGPIPE
    {
        int enabled = 1;
        if (setsockopt(helper_fd, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, sizeof(enabled)) < 0)
        {
            async_io_cleanup();
            return MY_FALSE;
        }
    }
#endif
    deadline = now + ASYNC_IO_START_SECONDS;
    while (!hello_received && helper_fd >= 0)
    {
        now = monotonic_seconds();
        if (now < 0 || now >= deadline)
            break;
        pump(deadline - now);
    }
    if (!hello_received || helper_fd < 0)
    {
        fprintf(stderr, "Async file helper failed its startup handshake.\n");
        async_io_cleanup();
        return MY_FALSE;
    }
    accepting = MY_TRUE;
    return MY_TRUE;
}

Bool
async_io_shutdown(void)
{
    double now = monotonic_seconds();
    double deadline = now + ASYNC_IO_SHUTDOWN_TIMEOUT;
    Bool success = MY_TRUE;
    accepting = MY_FALSE;
    drain_failed = MY_FALSE;
    while (queue_head)
    {
        if (helper_fd < 0)
        {
            success = MY_FALSE;
            stop_child();
        }
        async_io_dispatch();
        if (!queue_head)
            break;
        if (async_io_pending())
            continue;
        now = monotonic_seconds();
        if (now < 0 || now >= deadline)
        {
            success = MY_FALSE;
            writer_lost("shutdown deadline", ETIMEDOUT);
            continue;
        }
        pump(deadline - now);
    }
    async_io_cleanup();
    return success && !drain_failed;
}

/*-------------------------------------------------------------------------*/
void
async_io_remove_stale(void)
{
    async_request_t *request, *next;
    for (request = roots; request; request = next)
    {
        next = request->root_next;
        if (request->state == ASYNC_RETIRED && helper_pid <= 0)
        {
            cleanup_temporary(request);
            free_request(request);
            continue;
        }
        if (!valid_callback_object(&request->callback))
        {
            free_callback(&request->callback);
            init_empty_callback(&request->callback);
        }
        if (request->giver && (request->giver->flags & O_DESTRUCTED))
        {
            free_object(request->giver, "async stale command giver");
            request->giver = NULL;
        }
    }
}

#ifdef DEBUG
void
async_io_count_extra_refs(void)
{
    async_request_t *request;
    for (request = roots; request; request = request->root_next)
    {
        count_callback_extra_refs(&request->callback);
        if (request->giver)
            count_extra_ref_in_object(request->giver);
    }
}
#endif

#ifdef GC_SUPPORT
void
async_io_clear_refs(void)
{
    async_request_t *request;
    for (request = roots; request; request = request->root_next)
    {
        clear_memory_reference(request);
        if (request->payload)
            clear_memory_reference(request->payload);
        if (valid_callback_object(&request->callback))
            clear_ref_in_callback(&request->callback);
        if (request->giver)
            clear_object_ref(request->giver);
    }
}

void
async_io_count_refs(void)
{
    async_request_t *request;
    for (request = roots; request; request = request->root_next)
    {
        note_malloced_block_ref(request);
        if (request->payload)
            note_malloced_block_ref(request->payload);
        if (valid_callback_object(&request->callback))
            count_ref_in_callback(&request->callback);
        if (request->giver)
        {
            if (request->giver->flags & O_DESTRUCTED)
            {
                reference_destructed_object(request->giver);
                request->giver = NULL;
            }
            else
                request->giver->ref++;
        }
    }
}
#endif
#endif /* USE_ASYNC_IO */
