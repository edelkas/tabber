/*
 * md5.h - MD5 message digest (RFC 1321).
 *
 * Used to verify downloaded archives against the hash published in the digest.
 * MD5 is not collision-resistant and is used here purely as an integrity check
 * against corrupted or truncated downloads, never as a security measure.
 */
#ifndef TABBER_MD5_H
#define TABBER_MD5_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD5_DIGEST_LEN 16
#define MD5_HEX_LEN    32   /* characters, excluding the terminator */

typedef struct {
    unsigned int state[4];      /* A, B, C, D */
    unsigned int count[2];      /* message length in bits, low word first */
    unsigned char buffer[64];   /* partial block */
} md5_ctx;

void md5_init(md5_ctx *ctx);
void md5_update(md5_ctx *ctx, const void *data, size_t len);
void md5_final(md5_ctx *ctx, unsigned char digest[MD5_DIGEST_LEN]);

/* One-shot digest of a buffer, written as lowercase hex. */
void md5_hex(const void *data, size_t len, char out[MD5_HEX_LEN + 1]);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_MD5_H */
