#include <stdlib.h>
#include <string.h>

#include "deflate.h"
#include "gzip.h"
#include "inflate.h"
#include "util.h"

int gz_is_gzip(const void *data, size_t len)
{
    const unsigned char *p = data;

    return data && len >= 2 && p[0] == GZ_MAGIC_0 && p[1] == GZ_MAGIC_1;
}

/* Skips a NUL-terminated header field, returning the offset after it. */
static size_t skip_string(const unsigned char *p, size_t len, size_t at)
{
    while (at < len && p[at] != 0)
        at++;
    return at < len ? at + 1 : len + 1;   /* len + 1 signals "ran off the end" */
}

unsigned char *gz_extract(const void *data, size_t len, size_t *out_len,
                          char *err, size_t errsz)
{
    const unsigned char *p = data;
    size_t at = GZ_HEADER_SIZE, body;
    unsigned long isize, crc, got;
    unsigned char *out;
    unsigned flags;

    if (out_len)
        *out_len = 0;

    if (!gz_is_gzip(data, len) || len < GZ_HEADER_SIZE + GZ_TRAILER_SIZE) {
        err_set(err, errsz, "not a gzip stream");
        return NULL;
    }
    if (p[2] != GZ_METHOD_DEFLATE) {
        err_set(err, errsz, "gzip method %u is not deflate", (unsigned)p[2]);
        return NULL;
    }
    flags = p[3];
    if (flags & GZ_FLAG_RESERVED) {
        err_set(err, errsz, "gzip header uses reserved flags");
        return NULL;
    }

    /* Optional fields, in the order RFC 1952 lays them out. */
    if (flags & GZ_FLAG_EXTRA) {
        if (at + 2 > len) goto truncated;
        at += 2 + ((size_t)p[at] | ((size_t)p[at + 1] << 8));
    }
    if (flags & GZ_FLAG_NAME)
        at = skip_string(p, len, at);
    if (flags & GZ_FLAG_COMMENT)
        at = skip_string(p, len, at);
    if (flags & GZ_FLAG_HCRC)
        at += 2;
    if (at > len || len - at < GZ_TRAILER_SIZE)
        goto truncated;

    /* The trailer states what should come out, so the buffer fits exactly. */
    crc   = (unsigned long)p[len - 8] | ((unsigned long)p[len - 7] << 8) |
            ((unsigned long)p[len - 6] << 16) | ((unsigned long)p[len - 5] << 24);
    isize = (unsigned long)p[len - 4] | ((unsigned long)p[len - 3] << 8) |
            ((unsigned long)p[len - 2] << 16) | ((unsigned long)p[len - 1] << 24);
    body = len - at - GZ_TRAILER_SIZE;

    out = xmalloc((size_t)isize + 1);
    if (inflate_raw(p + at, body, out, (size_t)isize, err, errsz) != 0) {
        free(out);
        return NULL;
    }
    out[isize] = '\0';

    got = crc32_bytes(out, (size_t)isize);
    if (got != crc) {
        err_set(err, errsz, "gzip checksum mismatch (expected %08lx, got %08lx)", crc, got);
        free(out);
        return NULL;
    }

    if (out_len)
        *out_len = (size_t)isize;
    return out;

truncated:
    err_set(err, errsz, "gzip stream is truncated");
    return NULL;
}

/* Writes a 32-bit value the way both gzip's trailer fields are stored. */
static void gz_put32(byte_buf *out, unsigned long value)
{
    unsigned char b[4];

    b[0] = (unsigned char)(value & 0xFF);
    b[1] = (unsigned char)((value >> 8) & 0xFF);
    b[2] = (unsigned char)((value >> 16) & 0xFF);
    b[3] = (unsigned char)((value >> 24) & 0xFF);
    buf_append(out, b, sizeof b);
}

unsigned char *gz_compress(const void *data, size_t len, const char *name,
                           size_t *out_len)
{
    unsigned char header[GZ_HEADER_SIZE] = {
        GZ_MAGIC_0, GZ_MAGIC_1, GZ_METHOD_DEFLATE, 0,
        0, 0, 0, 0,          /* no modification time: the save has its own */
        0, GZ_OS_UNKNOWN
    };
    byte_buf out = {0};
    unsigned char *body;
    size_t body_len = 0;

    if (name && *name)
        header[3] = GZ_FLAG_NAME;
    buf_append(&out, header, sizeof header);
    if (name && *name)
        buf_append(&out, name, strlen(name) + 1);   /* NUL terminated */

    body = deflate_raw(data, len, &body_len);
    buf_append(&out, body, body_len);
    free(body);

    gz_put32(&out, crc32_bytes(data, len));
    gz_put32(&out, (unsigned long)(len & 0xFFFFFFFFu));   /* size, modulo 2^32 */

    return (unsigned char *)buf_finish(&out, out_len);
}
