/* Native protocol tests. Syscall faults exist only in this translation unit. */
#include "driver.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define REQUEST_SIZE 32
#define REPLY_SIZE 56
#define PATH_LIMIT 4096
#define WIRE_MAGIC UINT32_C(0x4c444149)
#define OP_APPEND 1
#define OP_OVERWRITE 2
#define OP_SAVE 3
#define REPLY_HELLO 1
#define REPLY_RESULT 2
#define REPLY_TEMP 3

enum fault_e
{
    FAULT_NONE,
    FAULT_SHORT_EINTR,
    FAULT_WRITE,
    FAULT_ZERO_WRITE,
    FAULT_CLOSE,
    FAULT_CLOSE_EINTR,
    FAULT_RENAME,
    FAULT_PROGRESS_REPLY,
    FAULT_RESULT_REPLY,
    FAULT_PAUSE_WRITE,
    FAULT_PAUSE_WRITE_FAIL,
    FAULT_READ,
    FAULT_HELLO_REPLY,
    FAULT_BROKEN_RESULT,
    FAULT_MODE,
    FAULT_STAT_EINTR,
    FAULT_STAT_ERROR
};

static uint32_t
wire_u32 (const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t
wire_u64 (const unsigned char *p)
{
    return ((uint64_t)wire_u32(p) << 32) | wire_u32(p + 4);
}

static void
wire_put32 (unsigned char *p, uint32_t n)
{
    p[0] = (unsigned char)(n >> 24);
    p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8);
    p[3] = (unsigned char)n;
}

static void
wire_put64 (unsigned char *p, uint64_t n)
{
    wire_put32(p, (uint32_t)(n >> 32));
    wire_put32(p + 4, (uint32_t)n);
}

#ifdef ASYNC_IO_WORKER_IN_TEST
static enum fault_e active_fault;
static int gate_fd;
static int file_write_count;
static int transport_write_count;
static int transport_read_count;
static int file_close_count;

static ssize_t
fault_write (int fd, const void *data, size_t size)
{
    if (fd == STDOUT_FILENO)
    {
        uint32_t kind = size >= REPLY_SIZE
                      ? wire_u32((const unsigned char *)data + 8) : 0;
        if (active_fault == FAULT_BROKEN_RESULT && kind == REPLY_RESULT)
        {
            if (shutdown(fd, SHUT_WR) < 0)
                _exit(94);
            return write(fd, data, size);
        }
        if ((active_fault == FAULT_PROGRESS_REPLY && kind == REPLY_TEMP)
         || (active_fault == FAULT_RESULT_REPLY && kind == REPLY_RESULT)
         || (active_fault == FAULT_HELLO_REPLY && kind == REPLY_HELLO))
        {
            errno = EPIPE;
            return -1;
        }
        if (active_fault == FAULT_SHORT_EINTR)
        {
            if (!transport_write_count++)
            {
                errno = EINTR;
                return -1;
            }
            if (size > 3)
                size = 3;
        }
    }
    else
    {
        if (active_fault == FAULT_PAUSE_WRITE
         || active_fault == FAULT_PAUSE_WRITE_FAIL)
        {
            char token;
            if (read(gate_fd, &token, 1) != 1)
                _exit(91);
            if (active_fault == FAULT_PAUSE_WRITE_FAIL)
            {
                errno = ENOSPC;
                return -1;
            }
            active_fault = FAULT_NONE;
        }
        if (active_fault == FAULT_WRITE)
        {
            if (file_write_count++)
            {
                errno = ENOSPC;
                return -1;
            }
            if (size > 2)
                size = 2;
        }
        if (active_fault == FAULT_ZERO_WRITE)
            return 0;
        if (active_fault == FAULT_SHORT_EINTR)
        {
            if (!file_write_count++)
            {
                errno = EINTR;
                return -1;
            }
            if (size > 2)
                size = 2;
        }
    }
    return write(fd, data, size);
}

static ssize_t
fault_read (int fd, void *data, size_t size)
{
    if (fd == STDIN_FILENO && active_fault == FAULT_READ)
    {
        errno = EIO;
        return -1;
    }
    if (fd == STDIN_FILENO && active_fault == FAULT_SHORT_EINTR)
    {
        if (!transport_read_count++)
        {
            errno = EINTR;
            return -1;
        }
        if (size > 2)
            size = 2;
    }
    return read(fd, data, size);
}

static int
fault_close (int fd)
{
    int rc = close(fd);
    if (fd > STDERR_FILENO
     && (active_fault == FAULT_CLOSE || active_fault == FAULT_CLOSE_EINTR))
    {
        if (file_close_count++)
            _exit(95);
        /* Simulate an error after the descriptor has already been released. */
        errno = active_fault == FAULT_CLOSE ? EIO : EINTR;
        return -1;
    }
    return rc;
}

static int
fault_rename (const char *from, const char *to)
{
    if (active_fault == FAULT_RENAME)
    {
        errno = EACCES;
        return -1;
    }
    return rename(from, to);
}

static int
fault_fchmod (int fd, mode_t mode)
{
    if (active_fault == FAULT_MODE)
    {
        errno = EPERM;
        return -1;
    }
    return fchmod(fd, mode);
}

static int
fault_fstat (int fd, struct stat *st)
{
    if (active_fault == FAULT_STAT_EINTR)
    {
        active_fault = FAULT_NONE;
        errno = EINTR;
        return -1;
    }
    if (active_fault == FAULT_STAT_ERROR)
    {
        errno = EIO;
        return -1;
    }
    return fstat(fd, st);
}

#define main async_io_worker_main
#define write fault_write
#define read fault_read
#define close fault_close
#define rename fault_rename
#define fchmod fault_fchmod
#define fstat fault_fstat
#include "../src/async_io_worker.c"
#undef main
#undef write
#undef read
#undef close
#undef rename
#undef fchmod
#undef fstat
#endif

typedef struct worker_s
{
    pid_t pid;
    int fd;
    int gate;
} worker_t;

typedef struct reply_s
{
    uint32_t kind;
    uint32_t error;
    uint64_t id;
    uint32_t result;
    uint32_t stage;
    uint64_t device;
    uint64_t inode;
    char path[PATH_LIMIT + 1];
} reply_t;

static const char *worker_path;
static const char *test_name;
static unsigned int checks;

static void
check (int condition, const char *description)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s: %s (errno %d)\n", test_name,
                description, errno);
        exit(1);
    }
    checks++;
}

static void
send_bytes (int fd, const void *data, size_t size, int fragmented)
{
    const unsigned char *p = data;
    while (size)
    {
        ssize_t n = write(fd, p, fragmented ? 1 : size);
        if (n < 0 && errno == EINTR)
            continue;
        check(n > 0, "send request bytes");
        size -= (size_t)n;
        p += n;
    }
}

static int
receive_bytes (int fd, void *data, size_t size)
{
    unsigned char *p = data;
    while (size)
    {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        size -= (size_t)n;
        p += n;
    }
    return 1;
}

static reply_t
receive_reply (worker_t *worker)
{
    unsigned char header[REPLY_SIZE];
    reply_t reply;
    uint32_t length;

    memset(&reply, 0, sizeof(reply));
    check(receive_bytes(worker->fd, header, sizeof(header)), "complete reply");
    check(wire_u32(header) == WIRE_MAGIC && wire_u32(header + 4) == 1,
          "reply magic and version");
    reply.kind = wire_u32(header + 8);
    reply.error = wire_u32(header + 12);
    reply.id = wire_u64(header + 16);
    length = wire_u32(header + 24);
    reply.result = wire_u32(header + 28);
    reply.device = wire_u64(header + 32);
    reply.inode = wire_u64(header + 40);
    reply.stage = wire_u32(header + 48);
    check(wire_u32(header + 52) == 0, "reserved reply field is zero");
    check(length <= PATH_LIMIT, "reply path length is bounded");
    check(reply.kind == REPLY_TEMP ? length > 0 : length == 0,
          "only progress replies carry a path");
    check(receive_bytes(worker->fd, reply.path, length), "complete progress path");
    check(!memchr(reply.path, '\0', length), "progress path has no NUL");
    return reply;
}

static worker_t
start_worker (enum fault_e fault, mode_t mask, int hello)
{
    int sockets[2], gate[2];
    worker_t worker;

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair");
    check(pipe(gate) == 0, "test synchronization pipe");
    worker.pid = fork();
    check(worker.pid >= 0, "fork worker");
    if (!worker.pid)
    {
        close(sockets[0]);
        close(gate[1]);
        if (dup2(sockets[1], STDIN_FILENO) < 0
         || dup2(sockets[1], STDOUT_FILENO) < 0)
            _exit(92);
        close(sockets[1]);
        umask(mask);
        signal(SIGPIPE, SIG_DFL);
#ifdef ASYNC_IO_WORKER_IN_TEST
        if (fault != FAULT_NONE)
        {
            active_fault = fault;
            gate_fd = gate[0];
            _exit(async_io_worker_main());
        }
#else
        (void)fault;
#endif
        close(gate[0]);
        execl(worker_path, worker_path, (char *)NULL);
        _exit(93);
    }
    close(sockets[1]);
    close(gate[0]);
    worker.fd = sockets[0];
    worker.gate = gate[1];
    if (hello)
    {
        reply_t reply = receive_reply(&worker);
        check(reply.kind == REPLY_HELLO && !reply.id && !reply.result
           && !reply.error && !reply.device && !reply.inode && !reply.stage,
              "worker sends a clean hello before accepting requests");
    }
    return worker;
}

static void
stop_worker (worker_t *worker, int success)
{
    int status;
    unsigned char byte;
    ssize_t count;
    check(shutdown(worker->fd, SHUT_WR) == 0, "finish request stream");
    count = read(worker->fd, &byte, 1);
    check(count == 0 || (!success && count < 0 && errno == ECONNRESET),
          "worker closes reply stream");
    check(waitpid(worker->pid, &status, 0) == worker->pid, "reap worker");
    check(WIFEXITED(status), "worker exits normally, including broken pipe");
    check(success ? WEXITSTATUS(status) == 0 : WEXITSTATUS(status) != 0,
          "worker exit status");
    close(worker->fd);
    close(worker->gate);
}

static void
make_header (unsigned char *header, unsigned int op, uint64_t id,
             uint32_t path_length, uint64_t payload_length)
{
    memset(header, 0, REQUEST_SIZE);
    wire_put32(header, WIRE_MAGIC);
    wire_put32(header + 4, 1);
    wire_put32(header + 8, op);
    wire_put32(header + 12, path_length);
    wire_put64(header + 16, payload_length);
    wire_put64(header + 24, id);
}

static void
send_request (worker_t *worker, unsigned int op, uint64_t id,
              const char *path, const void *payload, size_t length,
              int fragmented)
{
    unsigned char header[REQUEST_SIZE];
    make_header(header, op, id, (uint32_t)strlen(path), length);
    send_bytes(worker->fd, header, sizeof(header), fragmented);
    send_bytes(worker->fd, path, strlen(path), fragmented);
    send_bytes(worker->fd, payload, length, fragmented);
}

static void
expect_result (worker_t *worker, uint64_t id, unsigned int error)
{
    reply_t reply = receive_reply(worker);
    check(reply.kind == REPLY_RESULT && reply.id == id, "matching result ID");
    check(reply.result == (error ? 1U : 0U) && reply.error == error,
          "result success or failure and errno");
    check(!reply.device && !reply.inode, "result has no temporary identity");
    check(!error || reply.stage != 0, "failure identifies operation stage");
}

static void
put_file (const char *path, const void *data, size_t length)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    check(fd >= 0, "create fixture");
    send_bytes(fd, data, length, 0);
    check(close(fd) == 0, "close fixture");
}

static void
expect_file (const char *path, const void *data, size_t length)
{
    unsigned char actual[256];
    struct stat st;
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "open output file");
    check(fstat(fd, &st) == 0 && st.st_size == (off_t)length,
          "exact output file length");
    check(length <= sizeof(actual) && receive_bytes(fd, actual, length),
          "read output bytes");
    check(!memcmp(actual, data, length), "exact output including NUL bytes");
    check(close(fd) == 0, "close output file");
}

static void
expect_no_temps (void)
{
    DIR *dir = opendir(".");
    struct dirent *entry;
    check(dir != NULL, "scan test directory");
    while ((entry = readdir(dir)) != NULL)
        check(strncmp(entry->d_name, ".ldmud-async-", 13) != 0,
              "worker removed owned temporary files");
    check(closedir(dir) == 0, "close directory");
}

static void
test_operations (enum fault_e fault)
{
    static const unsigned char first[] = {'a', 0, 'b'};
    static const unsigned char appended[] = {'a', 0, 'b', 'c', 0, 'd'};
    worker_t worker;
    reply_t progress;
    struct stat st;

    test_name = fault ? "short writes and EINTR" : "real executable operations";
    unlink("text");
    unlink("object.o");
    worker = start_worker(fault, 0027, 1);
    send_request(&worker, OP_APPEND, UINT64_C(0xfedcba9876543210),
                 "text", first, sizeof(first), 1);
    expect_result(&worker, UINT64_C(0xfedcba9876543210), 0);
    expect_file("text", first, sizeof(first));
    check(stat("text", &st) == 0 && (st.st_mode & 0777) == 0640,
          "append creation mode respects inherited umask");
    send_request(&worker, OP_APPEND, 2, "text", appended + 3, 3, 0);
    expect_result(&worker, 2, 0);
    expect_file("text", appended, sizeof(appended));
    send_request(&worker, OP_OVERWRITE, 3, "text", "new", 3, 0);
    expect_result(&worker, 3, 0);
    expect_file("text", "new", 3);
    send_request(&worker, OP_APPEND, 4, "text", "", 0, 0);
    expect_result(&worker, 4, 0);
    expect_file("text", "new", 3);
    send_request(&worker, OP_OVERWRITE, 5, "text", "", 0, 0);
    expect_result(&worker, 5, 0);
    expect_file("text", "", 0);
    send_request(&worker, OP_SAVE, 6, "object.o", first, sizeof(first), 1);
    progress = receive_reply(&worker);
    check(progress.kind == REPLY_TEMP && progress.id == 6 && !progress.error
       && !progress.result, "save announces owned temporary file");
    expect_result(&worker, 6, 0);
    expect_file("object.o", first, sizeof(first));
    check(stat("object.o", &st) == 0 && (st.st_mode & 0777) == 0640,
          "save creation mode respects inherited umask");
    check((uint64_t)st.st_dev == progress.device
       && (uint64_t)st.st_ino == progress.inode, "rename preserves announced identity");
    send_request(&worker, OP_SAVE, 7, "object.o", "", 0, 0);
    progress = receive_reply(&worker);
    check(progress.kind == REPLY_TEMP && progress.id == 7, "empty save progress");
    expect_result(&worker, 7, 0);
    expect_file("object.o", "", 0);
    send_request(&worker, OP_APPEND, 8, "missing/child", "x", 1, 0);
    expect_result(&worker, 8, ENOENT);
    send_request(&worker, OP_SAVE, 9, "missing/child.o", "x", 1, 0);
    expect_result(&worker, 9, ENOENT);
    unlink("new-text");
    send_request(&worker, OP_OVERWRITE, 10, "new-text", "", 0, 0);
    expect_result(&worker, 10, 0);
    check(stat("new-text", &st) == 0 && (st.st_mode & 0777) == 0640,
          "overwrite creation mode respects inherited umask");
    send_request(&worker, OP_OVERWRITE, 11, "text", "1", 1, 0);
    send_request(&worker, OP_APPEND, 12, "text", "2", 1, 0);
    send_request(&worker, OP_APPEND, 13, "text", "3", 1, 0);
    expect_result(&worker, 11, 0);
    expect_result(&worker, 12, 0);
    expect_result(&worker, 13, 0);
    expect_file("text", "123", 3);
    stop_worker(&worker, 1);
    expect_no_temps();
}

static void
test_bad_frames (void)
{
    unsigned int variant;
    test_name = "malformed frames have no file side effects";
    for (variant = 0; variant < 12; variant++)
    {
        unsigned char header[REQUEST_SIZE];
        worker_t worker = start_worker(FAULT_NONE, 0022, 1);
        size_t header_length = sizeof(header);
        put_file("guard", "old", 3);
        make_header(header, OP_OVERWRITE, 1, 5, 3);
        switch (variant)
        {
        case 0: wire_put32(header, WIRE_MAGIC ^ 1); break;
        case 1: wire_put32(header + 4, 2); break;
        case 2: wire_put32(header + 8, 0); break;
        case 3: wire_put32(header + 8, 4); break;
        case 4: wire_put32(header + 12, 0); break;
        case 5: wire_put32(header + 12, PATH_LIMIT + 1); break;
        case 6: wire_put64(header + 16, UINT64_MAX); break;
        case 7: wire_put64(header + 16, ASYNC_IO_MAX_REQUEST_BYTES + UINT64_C(1)); break;
        case 8: wire_put64(header + 24, 0); break;
        case 9: header_length--; break;
        default: break;
        }
        send_bytes(worker.fd, header, header_length, 0);
        if (variant == 10)
            send_bytes(worker.fd, "gu\0rdxyz", 8, 0);
        if (variant == 11)
            send_bytes(worker.fd, "guardxy", 7, 0);
        stop_worker(&worker, 0);
        expect_file("guard", "old", 3);
    }
    /* Every partial header, path, and payload boundary is premature EOF. */
    for (variant = 1; variant < REQUEST_SIZE + 8; variant++)
    {
        unsigned char frame[REQUEST_SIZE + 8];
        worker_t worker = start_worker(FAULT_NONE, 0022, 1);
        make_header(frame, OP_OVERWRITE, 1, 5, 3);
        memcpy(frame + REQUEST_SIZE, "guardnew", 8);
        send_bytes(worker.fd, frame, variant, 0);
        stop_worker(&worker, 0);
        expect_file("guard", "old", 3);
    }
}

static void
test_path_bounds (void)
{
    char path[PATH_LIMIT + 1];
    worker_t worker;
    test_name = "bounded native and temporary paths";
    worker = start_worker(FAULT_NONE, 0022, 1);
    memset(path, 'x', PATH_LIMIT);
    path[PATH_LIMIT] = '\0';
    send_request(&worker, OP_APPEND, 1, path, "", 0, 0);
    expect_result(&worker, 1, ENAMETOOLONG);
    path[PATH_LIMIT - 2] = '/';
    send_request(&worker, OP_SAVE, 2, path, "", 0, 0);
    expect_result(&worker, 2, ENAMETOOLONG);
    stop_worker(&worker, 1);
}

#ifdef ASYNC_IO_WORKER_IN_TEST
static void
test_save_failure (enum fault_e fault, unsigned int expected_error)
{
    worker_t worker;
    reply_t progress;
    test_name = "failed atomic saves preserve destination";
    put_file("guard", "old", 3);
    worker = start_worker(fault, 0022, 1);
    send_request(&worker, OP_SAVE, 1, "guard", "replacement", 11, 0);
    progress = receive_reply(&worker);
    check(progress.kind == REPLY_TEMP && progress.id == 1, "save progress before failure");
    expect_result(&worker, 1, expected_error);
    expect_file("guard", "old", 3);
    check(access(progress.path, F_OK) == -1 && errno == ENOENT,
          "failed save removes announced temporary file");
    stop_worker(&worker, 1);
    expect_no_temps();
}

static void
test_identity_failure (void)
{
    worker_t worker;
    DIR *directory;
    struct dirent *entry;
    unsigned int found = 0;
    test_name = "unrecoverable temporary identity failure";
    expect_no_temps();
    put_file("guard", "old", 3);
    worker = start_worker(FAULT_STAT_ERROR, 0022, 1);
    send_request(&worker, OP_SAVE, 1, "guard", "new", 3, 0);
    expect_result(&worker, 1, EIO);
    expect_file("guard", "old", 3);
    stop_worker(&worker, 1);
    directory = opendir(".");
    check(directory != NULL, "open owned test directory");
    while ((entry = readdir(directory)) != NULL)
        if (strncmp(entry->d_name, ".ldmud-async-", 13) == 0)
        {
            found++;
            expect_file(entry->d_name, "", 0);
            /* The test owns the complete directory and may remove the
             * unannounced file. Production cannot assume that ownership. */
            check(unlink(entry->d_name) == 0, "remove test-owned orphan");
        }
    closedir(directory);
    check(found == 1, "only unidentified empty temporary remains");
}

static void
test_progress_identity (void)
{
    worker_t worker;
    reply_t progress;
    struct stat st;
    test_name = "progress identifies existing temp before payload writes";
    put_file("guard", "old", 3);
    worker = start_worker(FAULT_PAUSE_WRITE, 0077, 1);
    send_request(&worker, OP_SAVE, 1, "./guard", "new", 3, 0);
    progress = receive_reply(&worker);
    check(progress.kind == REPLY_TEMP && progress.id == 1, "progress kind and ID");
    check(stat(progress.path, &st) == 0, "announced temporary path exists");
    check(st.st_size == 0 && (uint64_t)st.st_dev == progress.device
       && (uint64_t)st.st_ino == progress.inode, "temporary identity before writing");
    check((st.st_mode & 0777) == 0600, "save honors restrictive inherited umask");
    expect_file("guard", "old", 3);
    send_bytes(worker.gate, "x", 1, 0);
    expect_result(&worker, 1, 0);
    expect_file("guard", "new", 3);
    stop_worker(&worker, 1);
    expect_no_temps();
}

static void
test_temp_replacement (enum fault_e fault)
{
    worker_t worker;
    reply_t progress;
    test_name = "failed save does not remove another file at its temp name";
    put_file("guard", "old", 3);
    worker = start_worker(fault, 0022, 1);
    send_request(&worker, OP_SAVE, 1, "guard", "new", 3, 0);
    progress = receive_reply(&worker);
    check(progress.kind == REPLY_TEMP, "owned temporary was announced");
    check(rename(progress.path, "displaced-temp") == 0, "move owned temp aside");
    put_file(progress.path, "unrelated", 9);
    send_bytes(worker.gate, "x", 1, 0);
    expect_result(&worker, 1, fault == FAULT_PAUSE_WRITE_FAIL ? ENOSPC : EIO);
    expect_file("guard", "old", 3);
    expect_file(progress.path, "unrelated", 9);
    stop_worker(&worker, 1);
    check(unlink(progress.path) == 0, "remove unrelated fixture");
    check(unlink("displaced-temp") == 0, "remove displaced fixture");
    expect_no_temps();
}

static void
test_transport_failures (void)
{
    worker_t worker;
    reply_t progress;
    test_name = "progress transport failure cleans temp and keeps old save";
    put_file("guard", "old", 3);
    worker = start_worker(FAULT_PROGRESS_REPLY, 0022, 1);
    send_request(&worker, OP_SAVE, 1, "guard", "new", 3, 0);
    stop_worker(&worker, 0);
    expect_file("guard", "old", 3);
    expect_no_temps();

    test_name = "result transport failure after rename is uncertain, never replayed";
    worker = start_worker(FAULT_RESULT_REPLY, 0022, 1);
    send_request(&worker, OP_SAVE, 1, "guard", "new", 3, 0);
    progress = receive_reply(&worker);
    check(progress.kind == REPLY_TEMP, "progress is delivered before lost result");
    stop_worker(&worker, 0);
    expect_file("guard", "new", 3);
    expect_no_temps();

    test_name = "append result loss does not repeat written bytes";
    worker = start_worker(FAULT_RESULT_REPLY, 0022, 1);
    send_request(&worker, OP_APPEND, 1, "guard", "+", 1, 0);
    stop_worker(&worker, 0);
    expect_file("guard", "new+", 4);

    test_name = "real broken reply socket does not terminate with SIGPIPE";
    worker = start_worker(FAULT_BROKEN_RESULT, 0022, 1);
    send_request(&worker, OP_APPEND, 1, "guard", "+", 1, 0);
    stop_worker(&worker, 0);
    expect_file("guard", "new++", 5);

    test_name = "read transport error is fatal";
    worker = start_worker(FAULT_READ, 0022, 1);
    stop_worker(&worker, 0);
    test_name = "hello transport error is fatal";
    worker = start_worker(FAULT_HELLO_REPLY, 0022, 0);
    stop_worker(&worker, 0);
}

static void
test_partial_text_failure (unsigned int op)
{
    worker_t worker;
    test_name = "text write failures expose only their written prefix";
    put_file("guard", "old", 3);
    worker = start_worker(FAULT_WRITE, 0022, 1);
    send_request(&worker, op, 1, "guard", "new-data", 8, 0);
    expect_result(&worker, 1, ENOSPC);
    stop_worker(&worker, 1);
    expect_file("guard", op == OP_APPEND ? "oldne" : "ne",
                op == OP_APPEND ? 5 : 2);
}
#endif

int
main (int argc, char **argv)
{
    worker_t worker;
    test_name = "test setup";
    check(argc == 3, "worker and scratch paths supplied");
    worker_path = argv[1];
    check(chdir(argv[2]) == 0, "enter private scratch directory");
    signal(SIGPIPE, SIG_IGN);
    alarm(30);
    test_name = "empty request stream";
    worker = start_worker(FAULT_NONE, 0022, 1);
    stop_worker(&worker, 1);
    test_operations(FAULT_NONE);
    test_bad_frames();
    test_path_bounds();
#ifdef ASYNC_IO_WORKER_IN_TEST
    test_operations(FAULT_SHORT_EINTR);
    test_operations(FAULT_STAT_EINTR);
    test_identity_failure();
    test_save_failure(FAULT_WRITE, ENOSPC);
    test_save_failure(FAULT_ZERO_WRITE, EIO);
    test_save_failure(FAULT_CLOSE, EIO);
    test_save_failure(FAULT_CLOSE_EINTR, EINTR);
    test_save_failure(FAULT_RENAME, EACCES);
    test_save_failure(FAULT_MODE, EPERM);
    test_progress_identity();
    test_temp_replacement(FAULT_PAUSE_WRITE_FAIL);
    test_temp_replacement(FAULT_PAUSE_WRITE);
    test_transport_failures();
    test_partial_text_failure(OP_APPEND);
    test_partial_text_failure(OP_OVERWRITE);
#endif
    printf("PASS async file worker: %u checks\n", checks);
    return 0;
}
