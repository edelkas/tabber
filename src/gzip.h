/*
 * gzip.h - Reading gzip streams (RFC 1952).
 *
 * Since the TEN++ update the game stores its savefile gzipped, so tabber has to
 * be able to unwrap one for a build that does not understand the format, and
 * to make one when it is asked to hand the game a compressed save.
 *
 * A gzip stream is a header, a raw DEFLATE stream, and a trailer carrying the
 * CRC-32 and the uncompressed size, which is what lets the output buffer be
 * sized exactly and the result checked independently. Streams with more than
 * one member are refused by that same check rather than half-read.
 */
#ifndef TABBER_GZIP_H
#define TABBER_GZIP_H

#include <stddef.h>

/* Header: magic, method, flags, and the fixed part's length. */
#define GZ_MAGIC_0          0x1F
#define GZ_MAGIC_1          0x8B
#define GZ_METHOD_DEFLATE   8
#define GZ_HEADER_SIZE      10
#define GZ_TRAILER_SIZE     8

/* Optional header fields, flagged in the header's fourth byte. */
#define GZ_FLAG_HCRC        0x02
#define GZ_FLAG_EXTRA       0x04
#define GZ_FLAG_NAME        0x08
#define GZ_FLAG_COMMENT     0x10
#define GZ_FLAG_RESERVED    0xE0   /* must be zero */

/* "Unknown" filesystem, which is what we claim in the streams we write. */
#define GZ_OS_UNKNOWN       255

/* Whether `data` begins with the gzip magic. */
int gz_is_gzip(const void *data, size_t len);

/*
 * Decompresses a gzip stream into a freshly allocated buffer of *out_len bytes
 * (plus a NUL terminator for safety), checking the CRC-32 and length the
 * trailer records. Returns NULL on failure with a reason in `err`.
 */
unsigned char *gz_extract(const void *data, size_t len, size_t *out_len,
                          char *err, size_t errsz);

/*
 * Wraps `data` in a gzip stream, deflating it on the way. `name` is recorded
 * as the original file name when it is given, as the game's own saves and the
 * previous installers' do. Returns a freshly allocated buffer of *out_len
 * bytes; the caller frees it.
 */
unsigned char *gz_compress(const void *data, size_t len, const char *name,
                           size_t *out_len);

#endif /* TABBER_GZIP_H */
