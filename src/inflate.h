/*
 * inflate.h - DEFLATE decompressor (RFC 1951).
 *
 * Raw deflate streams only, which is what ZIP entries store. The uncompressed
 * size is always known in advance from the archive, so the caller supplies an
 * exactly-sized output buffer and the decompressor never has to grow one.
 */
#ifndef TABBER_INFLATE_H
#define TABBER_INFLATE_H

#include <stddef.h>

/*
 * Decompresses `src_len` bytes into a buffer of exactly `dst_len` bytes.
 * Returns 0 on success, -1 on failure with a reason in `err`. Producing fewer
 * or more bytes than `dst_len` counts as a failure.
 */
int inflate_raw(const void *src, size_t src_len, void *dst, size_t dst_len,
                char *err, size_t errsz);

#endif /* TABBER_INFLATE_H */
