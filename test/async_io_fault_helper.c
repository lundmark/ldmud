/* A controllable test peer, never installed with the driver. */
#include "driver.h"
#include "async_io_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *scenario;

static void exact_read(void *data, size_t size)
{
    unsigned char *p = data;
    while (size)
    {
        ssize_t n = read(STDIN_FILENO, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (!n) _exit(0);
        if (n < 0) _exit(91);
        p += n;
        size -= n;
    }
}

static void exact_write(const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size)
    {
        ssize_t n = write(STDOUT_FILENO, p,
                          !strcmp(scenario, "fragment") ? 1 : size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(92);
        p += n;
        size -= n;
        if (!strcmp(scenario, "fragment"))
        {
            struct timespec delay = { 0, 1000000 };
            nanosleep(&delay, NULL);
        }
    }
}

static void send_reply(unsigned int kind, uint64_t id, unsigned int error,
                       const char *path, const struct stat *identity)
{
    unsigned char reply[AIO_REPLY_SIZE] = {0};
    aio_put_u32(reply, AIO_MAGIC);
    aio_put_u32(reply + 4, AIO_VERSION);
    aio_put_u32(reply + 8, kind);
    aio_put_u32(reply + 12, error);
    aio_put_u64(reply + 16, id);
    aio_put_u32(reply + 24, path ? strlen(path) : 0);
    aio_put_u32(reply + 28, error ? AIO_RESULT_ERROR : AIO_RESULT_OK);
    if (identity)
    {
        aio_put_u64(reply + 32, identity->st_dev);
        aio_put_u64(reply + 40, identity->st_ino);
    }
    if (!strcmp(scenario, "badhello") && kind == AIO_REPLY_HELLO)
        aio_put_u32(reply + 4, 99);
    if (!strcmp(scenario, "badlength") && kind == AIO_REPLY_RESULT)
        aio_put_u32(reply + 24, AIO_MAX_PATH + 1);
    if (!strcmp(scenario, "midreply") && kind == AIO_REPLY_RESULT)
    {
        exact_write(reply, 17);
        _exit(1);
    }
    exact_write(reply, sizeof(reply));
    if (path) exact_write(path, strlen(path));
}

static void ready_and_wait(void)
{
    int fd = open("peer-ready", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    char token;
    if (fd < 0 || write(fd, "ready", 5) != 5 || close(fd) < 0) _exit(93);
    fd = open("control", O_RDONLY);
    if (fd < 0 || read(fd, &token, 1) != 1) _exit(94);
    close(fd);
}

int main(void)
{
    uint64_t last_id = 0;
    scenario = getenv("ASYNC_TEST_SCENARIO");
    if (!scenario) return 90;
    signal(SIGPIPE, SIG_IGN);
    /* The harness deliberately passes this descriptor into the driver. */
    if (getenv("ASYNC_TEST_INHERITED_FD")
     && fcntl(atoi(getenv("ASYNC_TEST_INHERITED_FD")), F_GETFD) >= 0)
        return 95;
    {
        FILE *pidfile = fopen("peer-pid", "w");
        if (!pidfile) return 95;
        fprintf(pidfile, "%ld", (long)getpid());
        if (fclose(pidfile)) return 95;
    }
    if (!strcmp(scenario, "nohello"))
        for (;;) pause();
    send_reply(AIO_REPLY_HELLO, 0, 0, NULL, NULL);
    if (!strcmp(scenario, "stall") || !strcmp(scenario, "exit-ready"))
    {
        ready_and_wait();
        if (!strcmp(scenario, "exit-ready")) return 1;
    }
    if (!strcmp(scenario, "header-eof"))
    {
        unsigned char byte;
        exact_read(&byte, 1);
        return 1;
    }
    for (;;)
    {
        unsigned char header[AIO_REQUEST_SIZE];
        unsigned char *payload;
        char path[AIO_MAX_PATH + 1];
        uint64_t id, length;
        uint32_t path_length, operation;
        exact_read(header, sizeof(header));
        path_length = aio_get_u32(header + 12);
        length = aio_get_u64(header + 16);
        id = aio_get_u64(header + 24);
        operation = aio_get_u32(header + 8);
        if (aio_get_u32(header) != AIO_MAGIC || aio_get_u32(header + 4) != AIO_VERSION
         || !path_length || path_length > AIO_MAX_PATH
         || length > ASYNC_IO_MAX_REQUEST_BYTES || id != last_id + 1)
            return 96;
        last_id = id;
        exact_read(path, path_length);
        path[path_length] = '\0';
        payload = malloc(length ? (size_t)length : 1);
        if (!payload) return 97;
        exact_read(payload, length);
        free(payload);
        if (!strcmp(scenario, "lost")) return 1;
        if (!strcmp(scenario, "hang"))
            for (;;) pause();
        if (!strcmp(scenario, "temp-loss") || !strcmp(scenario, "temp-hang"))
        {
            char temporary[] = ".ldmud-async-XXXXXX";
            struct stat identity;
            int fd;
            if (operation != AIO_OP_SAVE) return 98;
            fd = mkstemp(temporary);
            if (fd < 0 || fstat(fd, &identity) < 0 || close(fd) < 0) return 99;
            send_reply(AIO_REPLY_TEMP, id, 0, temporary, &identity);
            if (!strcmp(scenario, "temp-hang"))
                for (;;) pause();
            ready_and_wait();
            return 1;
        }
        if (!strcmp(scenario, "committed-loss"))
        {
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd < 0 || write(fd, "committed", 9) != 9 || close(fd) < 0) return 99;
            return 1;
        }
        send_reply(AIO_REPLY_RESULT, !strcmp(scenario, "wrongid") ? id + 1 : id,
                   0, NULL, NULL);
    }
}
