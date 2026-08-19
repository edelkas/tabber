/*
 * deflate.h - DEFLATE compressor (RFC 1951).
 *
 * The counterpart of inflate.c, and deliberately the plain version of the
 * algorithm: a 32 KB window, a hash chain over three-byte sequences, greedy
 * matching, and one Huffman-coded block per few thousand symbols. Each block
 * is emitted in whichever of the three forms the format offers comes out
 * smallest — stored, fixed codes, or codes built for that block — so the
 * output is never larger than the input by more than a block header.
 *
 * Nothing here is tuned for speed beyond the chain limits below; it exists so
 * the tool can hand the game a gzipped savefile of its own making.
 */
#ifndef TABBER_DEFLATE_H
#define TABBER_DEFLATE_H

#include <stddef.h>

/* Fixed by the format: the window, and what counts as a match. */
#define DEFLATE_WINDOW       32768u
#define DEFLATE_MIN_MATCH    3
#define DEFLATE_MAX_MATCH    258

/*
 * How hard the matcher looks: how many candidates it walks back through, and
 * the match length at which it stops looking for a better one.
 */
#define DEFLATE_MAX_CHAIN    32
#define DEFLATE_NICE_MATCH   128

/* Hash of DEFLATE_MIN_MATCH bytes, used to find those candidates. */
#define DEFLATE_HASH_BITS    15
#define DEFLATE_HASH_SIZE    (1u << DEFLATE_HASH_BITS)

/*
 * A block ends at whichever of these comes first. The byte limit keeps a
 * stored block available as a fallback, since one cannot hold more than 65535
 * bytes.
 */
#define DEFLATE_BLOCK_SYMS   16384
#define DEFLATE_BLOCK_BYTES  65535

/*
 * Compresses to a raw DEFLATE stream in a freshly allocated buffer of
 * *out_len bytes. Never fails other than by aborting on out-of-memory, which
 * is what the rest of the tool does too. Caller frees.
 */
unsigned char *deflate_raw(const void *data, size_t len, size_t *out_len);

#endif /* TABBER_DEFLATE_H */
