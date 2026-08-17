#include <string.h>

#include "md5.h"

/* Per-round shift amounts (RFC 1321, section 3.4). */
#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

/* The four nonlinear round functions. */
#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, s, ac)             \
    do {                                          \
        (a) += f((b), (c), (d)) + (x) + (ac);     \
        (a) = ROTL((a), (s));                     \
        (a) += (b);                               \
    } while (0)

/* Reads 16 little-endian words out of a 64-byte block. */
static void md5_decode(unsigned int out[16], const unsigned char *in)
{
    int i;

    for (i = 0; i < 16; i++) {
        out[i] = (unsigned int)in[i * 4] |
                 ((unsigned int)in[i * 4 + 1] << 8) |
                 ((unsigned int)in[i * 4 + 2] << 16) |
                 ((unsigned int)in[i * 4 + 3] << 24);
    }
}

/* Writes words back as little-endian bytes. */
static void md5_encode(unsigned char *out, const unsigned int *in, size_t len)
{
    size_t i, j;

    for (i = 0, j = 0; j < len; i++, j += 4) {
        out[j]     = (unsigned char)(in[i] & 0xFF);
        out[j + 1] = (unsigned char)((in[i] >> 8) & 0xFF);
        out[j + 2] = (unsigned char)((in[i] >> 16) & 0xFF);
        out[j + 3] = (unsigned char)((in[i] >> 24) & 0xFF);
    }
}

/* The core compression function, operating on one 64-byte block. */
static void md5_transform(unsigned int state[4], const unsigned char block[64])
{
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3], x[16];

    md5_decode(x, block);

    /* Round 1 */
    STEP(F, a, b, c, d, x[ 0], S11, 0xd76aa478);
    STEP(F, d, a, b, c, x[ 1], S12, 0xe8c7b756);
    STEP(F, c, d, a, b, x[ 2], S13, 0x242070db);
    STEP(F, b, c, d, a, x[ 3], S14, 0xc1bdceee);
    STEP(F, a, b, c, d, x[ 4], S11, 0xf57c0faf);
    STEP(F, d, a, b, c, x[ 5], S12, 0x4787c62a);
    STEP(F, c, d, a, b, x[ 6], S13, 0xa8304613);
    STEP(F, b, c, d, a, x[ 7], S14, 0xfd469501);
    STEP(F, a, b, c, d, x[ 8], S11, 0x698098d8);
    STEP(F, d, a, b, c, x[ 9], S12, 0x8b44f7af);
    STEP(F, c, d, a, b, x[10], S13, 0xffff5bb1);
    STEP(F, b, c, d, a, x[11], S14, 0x895cd7be);
    STEP(F, a, b, c, d, x[12], S11, 0x6b901122);
    STEP(F, d, a, b, c, x[13], S12, 0xfd987193);
    STEP(F, c, d, a, b, x[14], S13, 0xa679438e);
    STEP(F, b, c, d, a, x[15], S14, 0x49b40821);

    /* Round 2 */
    STEP(G, a, b, c, d, x[ 1], S21, 0xf61e2562);
    STEP(G, d, a, b, c, x[ 6], S22, 0xc040b340);
    STEP(G, c, d, a, b, x[11], S23, 0x265e5a51);
    STEP(G, b, c, d, a, x[ 0], S24, 0xe9b6c7aa);
    STEP(G, a, b, c, d, x[ 5], S21, 0xd62f105d);
    STEP(G, d, a, b, c, x[10], S22, 0x02441453);
    STEP(G, c, d, a, b, x[15], S23, 0xd8a1e681);
    STEP(G, b, c, d, a, x[ 4], S24, 0xe7d3fbc8);
    STEP(G, a, b, c, d, x[ 9], S21, 0x21e1cde6);
    STEP(G, d, a, b, c, x[14], S22, 0xc33707d6);
    STEP(G, c, d, a, b, x[ 3], S23, 0xf4d50d87);
    STEP(G, b, c, d, a, x[ 8], S24, 0x455a14ed);
    STEP(G, a, b, c, d, x[13], S21, 0xa9e3e905);
    STEP(G, d, a, b, c, x[ 2], S22, 0xfcefa3f8);
    STEP(G, c, d, a, b, x[ 7], S23, 0x676f02d9);
    STEP(G, b, c, d, a, x[12], S24, 0x8d2a4c8a);

    /* Round 3 */
    STEP(H, a, b, c, d, x[ 5], S31, 0xfffa3942);
    STEP(H, d, a, b, c, x[ 8], S32, 0x8771f681);
    STEP(H, c, d, a, b, x[11], S33, 0x6d9d6122);
    STEP(H, b, c, d, a, x[14], S34, 0xfde5380c);
    STEP(H, a, b, c, d, x[ 1], S31, 0xa4beea44);
    STEP(H, d, a, b, c, x[ 4], S32, 0x4bdecfa9);
    STEP(H, c, d, a, b, x[ 7], S33, 0xf6bb4b60);
    STEP(H, b, c, d, a, x[10], S34, 0xbebfbc70);
    STEP(H, a, b, c, d, x[13], S31, 0x289b7ec6);
    STEP(H, d, a, b, c, x[ 0], S32, 0xeaa127fa);
    STEP(H, c, d, a, b, x[ 3], S33, 0xd4ef3085);
    STEP(H, b, c, d, a, x[ 6], S34, 0x04881d05);
    STEP(H, a, b, c, d, x[ 9], S31, 0xd9d4d039);
    STEP(H, d, a, b, c, x[12], S32, 0xe6db99e5);
    STEP(H, c, d, a, b, x[15], S33, 0x1fa27cf8);
    STEP(H, b, c, d, a, x[ 2], S34, 0xc4ac5665);

    /* Round 4 */
    STEP(I, a, b, c, d, x[ 0], S41, 0xf4292244);
    STEP(I, d, a, b, c, x[ 7], S42, 0x432aff97);
    STEP(I, c, d, a, b, x[14], S43, 0xab9423a7);
    STEP(I, b, c, d, a, x[ 5], S44, 0xfc93a039);
    STEP(I, a, b, c, d, x[12], S41, 0x655b59c3);
    STEP(I, d, a, b, c, x[ 3], S42, 0x8f0ccc92);
    STEP(I, c, d, a, b, x[10], S43, 0xffeff47d);
    STEP(I, b, c, d, a, x[ 1], S44, 0x85845dd1);
    STEP(I, a, b, c, d, x[ 8], S41, 0x6fa87e4f);
    STEP(I, d, a, b, c, x[15], S42, 0xfe2ce6e0);
    STEP(I, c, d, a, b, x[ 6], S43, 0xa3014314);
    STEP(I, b, c, d, a, x[13], S44, 0x4e0811a1);
    STEP(I, a, b, c, d, x[ 4], S41, 0xf7537e82);
    STEP(I, d, a, b, c, x[11], S42, 0xbd3af235);
    STEP(I, c, d, a, b, x[ 2], S43, 0x2ad7d2bb);
    STEP(I, b, c, d, a, x[ 9], S44, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void md5_init(md5_ctx *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void md5_update(md5_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *input = data;
    size_t i, index, part;

    /* Byte offset inside the pending block, from the bit counter. */
    index = (size_t)((ctx->count[0] >> 3) & 0x3F);

    ctx->count[0] += (unsigned int)(len << 3);
    if (ctx->count[0] < (unsigned int)(len << 3))
        ctx->count[1]++;              /* carry into the high word */
    ctx->count[1] += (unsigned int)(len >> 29);

    part = 64 - index;
    if (len >= part) {
        memcpy(&ctx->buffer[index], input, part);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part; i + 63 < len; i += 64)
            md5_transform(ctx->state, &input[i]);
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[index], &input[i], len - i);
}

void md5_final(md5_ctx *ctx, unsigned char digest[MD5_DIGEST_LEN])
{
    static const unsigned char padding[64] = { 0x80 };   /* rest is zero */
    unsigned char bits[8];
    size_t index, pad_len;

    md5_encode(bits, ctx->count, 8);

    /* Pad to 56 bytes mod 64, then append the length. */
    index = (size_t)((ctx->count[0] >> 3) & 0x3F);
    pad_len = (index < 56) ? (56 - index) : (120 - index);
    md5_update(ctx, padding, pad_len);
    md5_update(ctx, bits, 8);

    md5_encode(digest, ctx->state, MD5_DIGEST_LEN);
    memset(ctx, 0, sizeof(*ctx));
}

void md5_hex(const void *data, size_t len, char out[MD5_HEX_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[MD5_DIGEST_LEN];
    md5_ctx ctx;
    int i;

    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, digest);

    for (i = 0; i < MD5_DIGEST_LEN; i++) {
        out[i * 2]     = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    out[MD5_HEX_LEN] = '\0';
}
