/*--------------------------------------------------------------------------
 * Standalone asynchronous file writer. This process owns no LPC/driver state
 * and links only libc. Its private blocking stream is stdin for requests and
 * stdout for replies; one complete bounded request is applied at a time.
 *--------------------------------------------------------------------------
 */

#include "driver.h"
#include "async_io_protocol.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define AIO_SAVE_MODE 0640
#define AIO_TEXT_MODE 0666
#define AIO_TEMP_NAME ".ldmud-async-XXXXXX"

/*-------------------------------------------------------------------------*/
static int
worker_read (void *buffer, size_t length)

/* Return 1 for a complete read, 0 for EOF before any bytes, -1 for a partial
 * read or error. Only the request-header caller permits a clean EOF.
 */

{
    unsigned char *p = buffer;
    size_t remaining = length;

    while (remaining)
    {
        ssize_t count = read(STDIN_FILENO, p, remaining);
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!count)
            return remaining == length ? 0 : -1;
        p += count;
        remaining -= (size_t)count;
    }
    return 1;
} /* worker_read() */

/*-------------------------------------------------------------------------*/
static int
worker_write (int fd, const void *buffer, size_t length)

/* A zero-progress write is an error, including on unusual files/devices. */

{
    const unsigned char *p = buffer;

    while (length)
    {
        ssize_t count = write(fd, p, length);
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (!count)
        {
            errno = EIO;
            return 0;
        }
        p += count;
        length -= (size_t)count;
    }
    return 1;
} /* worker_write() */

/*-------------------------------------------------------------------------*/
static int
worker_reply (uint32_t kind, uint64_t id, int error, uint32_t stage,
              const char *path, const struct stat *identity)
{
    unsigned char header[AIO_REPLY_SIZE];
    size_t path_length = path ? strlen(path) : 0;

    if (path_length > AIO_MAX_PATH)
    {
        errno = ENAMETOOLONG;
        return 0;
    }
    memset(header, 0, sizeof(header));
    aio_put_u32(header, AIO_MAGIC);
    aio_put_u32(header + 4, AIO_VERSION);
    aio_put_u32(header + 8, kind);
    aio_put_u32(header + 12, (uint32_t)error);
    aio_put_u64(header + 16, id);
    aio_put_u32(header + 24, (uint32_t)path_length);
    aio_put_u32(header + 28, error ? AIO_RESULT_ERROR : AIO_RESULT_OK);
    if (identity)
    {
        aio_put_u64(header + 32, (uint64_t)identity->st_dev);
        aio_put_u64(header + 40, (uint64_t)identity->st_ino);
    }
    aio_put_u32(header + 48, stage);

    return worker_write(STDOUT_FILENO, header, sizeof(header))
        && worker_write(STDOUT_FILENO, path, path_length);
} /* worker_reply() */

/*-------------------------------------------------------------------------*/
static void
worker_remove_temp (const char *path, const struct stat *identity)

/* Never remove a replacement someone else installed under our temp name. */

{
    struct stat current;

    if (lstat(path, &current) == 0
     && current.st_dev == identity->st_dev
     && current.st_ino == identity->st_ino)
        unlink(path);
} /* worker_remove_temp() */

/*-------------------------------------------------------------------------*/
static int
worker_save (uint64_t id, const char *path, const void *payload,
             size_t length, mode_t creation_mask)
{
    char temporary[AIO_MAX_PATH + 1];
    const char *slash = strrchr(path, '/');
    size_t directory_length = slash ? (size_t)(slash - path) + 1 : 0;
    struct stat identity;
    int fd;
    int error = 0;
    uint32_t stage = AIO_STAGE_NONE;

    if (directory_length + sizeof(AIO_TEMP_NAME) > sizeof(temporary))
        return worker_reply(AIO_REPLY_RESULT, id, ENAMETOOLONG,
                            AIO_STAGE_TEMP, NULL, NULL);

    memcpy(temporary, path, directory_length);
    memcpy(temporary + directory_length, AIO_TEMP_NAME, sizeof(AIO_TEMP_NAME));
    fd = mkstemp(temporary);
    if (fd < 0)
        return worker_reply(AIO_REPLY_RESULT, id, errno,
                            AIO_STAGE_TEMP, NULL, NULL);

    do
    {
        error = fstat(fd, &identity) < 0 ? errno : 0;
    } while (error == EINTR);
    if (error)
    {
        close(fd);
        /* Without an identity, do not guess which directory entry to unlink. */
        fprintf(stderr, "Async file writer: cannot identify temporary '%s' "
                        "(errno %d); empty file left for operator cleanup.\n",
                        temporary, error);
        return worker_reply(AIO_REPLY_RESULT, id, error,
                            AIO_STAGE_STAT, NULL, NULL);
    }

    /* Publish ownership before any payload write. If transport is lost, stop
     * without applying this or any later request; the driver must not replay.
     */
    if (!worker_reply(AIO_REPLY_TEMP, id, 0, AIO_STAGE_TEMP,
                      temporary, &identity))
    {
        close(fd);
        worker_remove_temp(temporary, &identity);
        return 0;
    }

    /* mkstemp starts at 0600. Apply the existing object-save creation policy,
     * honoring the inherited mask even though fchmod itself ignores umask.
     */
    if (fchmod(fd, AIO_SAVE_MODE & ~creation_mask) < 0)
    {
        error = errno;
        stage = AIO_STAGE_MODE;
    }
    else if (!worker_write(fd, payload, length))
    {
        error = errno;
        stage = AIO_STAGE_WRITE;
    }

    /* close(EINTR) can already have released fd. Never retry close and risk
     * closing a reused descriptor; an unconfirmed close forbids rename.
     */
    if (close(fd) < 0 && !error)
    {
        error = errno;
        stage = AIO_STAGE_CLOSE;
    }
    if (!error)
    {
        struct stat current;
        int result;
        do
        {
            result = lstat(temporary, &current);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            error = errno;
        else if (current.st_dev != identity.st_dev
              || current.st_ino != identity.st_ino)
            error = EIO;
        if (error)
            stage = AIO_STAGE_STAT;
        /* Reject a displaced pathname. This is not a lock against concurrent
         * external directory changes between the check and rename. */
    }
    if (!error && rename(temporary, path) < 0)
    {
        error = errno;
        stage = AIO_STAGE_RENAME;
    }
    if (error)
        worker_remove_temp(temporary, &identity);

    return worker_reply(AIO_REPLY_RESULT, id, error, stage, NULL, NULL);
} /* worker_save() */

/*-------------------------------------------------------------------------*/
static int
worker_apply (uint32_t operation, uint64_t id, const char *path,
              const void *payload, size_t length, mode_t creation_mask)
{
    int fd;
    int error = 0;
    uint32_t stage = AIO_STAGE_NONE;

    if (operation == AIO_OP_SAVE)
        return worker_save(id, path, payload, length, creation_mask);

    fd = open(path, O_WRONLY | O_CREAT
                    | (operation == AIO_OP_APPEND ? O_APPEND : O_TRUNC),
              AIO_TEXT_MODE);
    if (fd < 0)
        return worker_reply(AIO_REPLY_RESULT, id, errno,
                            AIO_STAGE_OPEN, NULL, NULL);

    if (!worker_write(fd, payload, length))
    {
        error = errno;
        stage = AIO_STAGE_WRITE;
    }
    if (close(fd) < 0 && !error)
    {
        error = errno;
        stage = AIO_STAGE_CLOSE;
    }
    return worker_reply(AIO_REPLY_RESULT, id, error, stage, NULL, NULL);
} /* worker_apply() */

/*-------------------------------------------------------------------------*/
int
main (void)
{
    unsigned char header[AIO_REQUEST_SIZE];
    char path[AIO_MAX_PATH + 1];
    mode_t creation_mask;

    /* Broken reply transport is a failed request, never a signal death. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return EXIT_FAILURE;
    creation_mask = umask(0);
    umask(creation_mask);
    if (!worker_reply(AIO_REPLY_HELLO, 0, 0, AIO_STAGE_NONE, NULL, NULL))
        return EXIT_FAILURE;

    for (;;)
    {
        uint32_t operation;
        uint32_t path_length;
        uint64_t payload_length;
        uint64_t id;
        unsigned char *payload;
        int result;

        result = worker_read(header, sizeof(header));
        if (result == 0)
            return EXIT_SUCCESS;
        if (result < 0)
            return EXIT_FAILURE;

        operation = aio_get_u32(header + 8);
        path_length = aio_get_u32(header + 12);
        payload_length = aio_get_u64(header + 16);
        id = aio_get_u64(header + 24);
        if (aio_get_u32(header) != AIO_MAGIC
         || aio_get_u32(header + 4) != AIO_VERSION
         || (operation != AIO_OP_APPEND && operation != AIO_OP_OVERWRITE
             && operation != AIO_OP_SAVE)
         || !id || !path_length || path_length > AIO_MAX_PATH
         || payload_length > ASYNC_IO_MAX_REQUEST_BYTES
         || payload_length > SIZE_MAX)
            return EXIT_FAILURE;

        if (worker_read(path, path_length) != 1
         || memchr(path, '\0', path_length))
            return EXIT_FAILURE;
        path[path_length] = '\0';

        /* Finish receiving before truncating/creating any destination. A
         * malformed or incomplete request must have no file side effects.
         */
        payload = malloc(payload_length ? (size_t)payload_length : 1);
        if (!payload)
            return EXIT_FAILURE;
        if (worker_read(payload, (size_t)payload_length) != 1)
        {
            free(payload);
            return EXIT_FAILURE;
        }
        result = worker_apply(operation, id, path, payload,
                              (size_t)payload_length, creation_mask);
        free(payload);
        if (!result)
            return EXIT_FAILURE;
    }
} /* main() */
