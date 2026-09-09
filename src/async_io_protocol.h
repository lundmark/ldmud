#ifndef ASYNC_IO_PROTOCOL_H__
#define ASYNC_IO_PROTOCOL_H__ 1

/* Private driver/worker protocol. Integer fields are unsigned, big endian;
 * frames never contain native structs, pointers, or implicit padding.
 */
#include "driver.h"
#include <stdint.h>

#define AIO_MAGIC        UINT32_C(0x4c444149)
#define AIO_VERSION      1
#define AIO_REQUEST_SIZE 32
#define AIO_REPLY_SIZE   56
#define AIO_MAX_PATH     4096

#define AIO_OP_APPEND    1
#define AIO_OP_OVERWRITE 2
#define AIO_OP_SAVE      3

#define AIO_REPLY_HELLO  1
#define AIO_REPLY_RESULT 2
#define AIO_REPLY_TEMP   3

#define AIO_RESULT_OK    0
#define AIO_RESULT_ERROR 1

#define AIO_STAGE_NONE   0
#define AIO_STAGE_OPEN   1
#define AIO_STAGE_TEMP   2
#define AIO_STAGE_STAT   3
#define AIO_STAGE_MODE   4
#define AIO_STAGE_WRITE  5
#define AIO_STAGE_CLOSE  6
#define AIO_STAGE_RENAME 7
#define AIO_STAGE_ALLOC  8

/* Request (32 bytes): magic:u32, version:u32, operation:u32,
 * path length:u32, payload length:u64, nonzero request ID:u64.
 * The header is followed by path bytes (no NUL), then exact payload bytes.
 * ASYNC_IO_MAX_REQUEST_BYTES bounds the payload, independently of the path.
 *
 * Reply (56 bytes): magic:u32, version:u32, kind:u32, errno:u32,
 * request ID:u64, path length:u32, result:u32, device:u64, inode:u64,
 * stage:u32, reserved:u32 (zero).
 * Only TEMP replies have a path, appended without a NUL, and an identity.
 * HELLO has all fields after kind zero. RESULT has a request ID and the
 * operation's result, errno and stage. A lost result must never be replayed.
 */

static INLINE uint32_t
aio_get_u32 (const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
} /* aio_get_u32() */

static INLINE uint64_t
aio_get_u64 (const unsigned char *p)
{
    return ((uint64_t)aio_get_u32(p) << 32) | aio_get_u32(p + 4);
} /* aio_get_u64() */

static INLINE void
aio_put_u32 (unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
} /* aio_put_u32() */

static INLINE void
aio_put_u64 (unsigned char *p, uint64_t value)
{
    aio_put_u32(p, (uint32_t)(value >> 32));
    aio_put_u32(p + 4, (uint32_t)value);
} /* aio_put_u64() */

#endif /* ASYNC_IO_PROTOCOL_H__ */
