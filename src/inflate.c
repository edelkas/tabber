#include <string.h>

#include "inflate.h"
#include "util.h"

/* Format limits from RFC 1951. */
#define MAX_BITS      15    /* longest Huffman code                     */
#define MAX_LCODES    286   /* literal/length codes                     */
#define MAX_DCODES    30    /* distance codes                           */
#define FIXED_LCODES  288   /* literal/length codes in fixed blocks     */
#define CODE_LENGTHS  19    /* codes describing the code length alphabet */
#define END_OF_BLOCK  256   /* literal/length symbol ending a block     */

typedef struct {
    const unsigned char *in;
    size_t in_len, in_pos;
    unsigned long bit_buf;   /* bits read but not yet consumed */
    int bit_cnt;
    unsigned char *out;
    size_t out_len, out_pos;
    char *err;
    size_t errsz;
} inflate_state;

/*
 * Canonical Huffman decoding table: count[n] holds how many symbols use codes
 * of n bits, and symbol[] lists the symbols ordered by code.
 */
typedef struct {
    short *count;    /* MAX_BITS + 1 entries */
    short *symbol;
} huff_table;

/* Base lengths and extra bits for the length codes (symbols 257..285). */
static const short length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* Base distances and extra bits for the distance codes. */
static const short dist_base[MAX_DCODES] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short dist_extra[MAX_DCODES] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Order in which the code length code lengths are stored. */
static const short length_order[CODE_LENGTHS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ---- Bit input --------------------------------------------------------- */

/* Returns the next `need` bits (LSB first), or -1 when the input runs out. */
static int inf_bits(inflate_state *s, int need)
{
    long value = (long)s->bit_buf;

    while (s->bit_cnt < need) {
        if (s->in_pos >= s->in_len)
            return -1;
        value |= (long)s->in[s->in_pos++] << s->bit_cnt;
        s->bit_cnt += 8;
    }
    s->bit_buf = (unsigned long)(value >> need);
    s->bit_cnt -= need;
    return (int)(value & ((1L << need) - 1));
}

/* ---- Huffman ----------------------------------------------------------- */

/*
 * Builds a decoding table from a list of code lengths. Returns 0 when the code
 * is complete, a positive count of unused codes when it is incomplete, and a
 * negative value when it is over-subscribed.
 */
static int inf_build(huff_table *table, const short *lengths, int n)
{
    short offsets[MAX_BITS + 1];
    int symbol, len, left;

    for (len = 0; len <= MAX_BITS; len++)
        table->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        table->count[lengths[symbol]]++;
    if (table->count[0] == n)
        return 0;   /* no codes at all: nothing to decode, but not an error */

    /* Check for an over-subscribed or incomplete set. */
    left = 1;
    for (len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= table->count[len];
        if (left < 0)
            return left;
    }

    /* Sort the symbols by code length, then by symbol within each length. */
    offsets[1] = 0;
    for (len = 1; len < MAX_BITS; len++)
        offsets[len + 1] = (short)(offsets[len] + table->count[len]);
    for (symbol = 0; symbol < n; symbol++) {
        if (lengths[symbol])
            table->symbol[offsets[lengths[symbol]]++] = (short)symbol;
    }
    return left;
}

/* Decodes one symbol, walking the canonical code one bit at a time. */
static int inf_decode(inflate_state *s, const huff_table *table)
{
    int len, code = 0, first = 0, index = 0;

    for (len = 1; len <= MAX_BITS; len++) {
        int bit = inf_bits(s, 1);
        if (bit < 0)
            return -1;
        code |= bit;
        if (code - table->count[len] < first)   /* the code fits this length */
            return table->symbol[index + (code - first)];
        index += table->count[len];
        first = (first + table->count[len]) << 1;
        code <<= 1;
    }
    return -1;   /* ran past the longest possible code */
}

/* ---- Block types ------------------------------------------------------- */

/* Uncompressed block: byte-aligned, with a length and its complement. */
static int inf_stored(inflate_state *s)
{
    unsigned len, nlen;

    s->bit_buf = 0;   /* discard bits to the next byte boundary */
    s->bit_cnt = 0;

    if (s->in_len - s->in_pos < 4) {
        err_set(s->err, s->errsz, "truncated stored block header");
        return -1;
    }
    len  = (unsigned)s->in[s->in_pos] | ((unsigned)s->in[s->in_pos + 1] << 8);
    nlen = (unsigned)s->in[s->in_pos + 2] | ((unsigned)s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF)) {
        err_set(s->err, s->errsz, "corrupt stored block length");
        return -1;
    }
    if (s->in_len - s->in_pos < len) {
        err_set(s->err, s->errsz, "truncated stored block data");
        return -1;
    }
    if (s->out_len - s->out_pos < len) {
        err_set(s->err, s->errsz, "output larger than the declared size");
        return -1;
    }

    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

/* Decodes literal/length and distance symbols until the end-of-block marker. */
static int inf_codes(inflate_state *s, const huff_table *lencode, const huff_table *distcode)
{
    for (;;) {
        int symbol = inf_decode(s, lencode);
        int extra;
        size_t len, dist;

        if (symbol < 0) {
            err_set(s->err, s->errsz, "invalid literal/length code");
            return -1;
        }

        if (symbol < END_OF_BLOCK) {                 /* literal byte */
            if (s->out_pos >= s->out_len) {
                err_set(s->err, s->errsz, "output larger than the declared size");
                return -1;
            }
            s->out[s->out_pos++] = (unsigned char)symbol;
            continue;
        }
        if (symbol == END_OF_BLOCK)
            return 0;

        /* A length/distance pair: copy from earlier in the output. */
        symbol -= END_OF_BLOCK + 1;
        if (symbol >= 29) {
            err_set(s->err, s->errsz, "invalid length code");
            return -1;
        }
        extra = inf_bits(s, length_extra[symbol]);
        if (extra < 0) {
            err_set(s->err, s->errsz, "truncated length code");
            return -1;
        }
        len = (size_t)length_base[symbol] + (size_t)extra;

        symbol = inf_decode(s, distcode);
        if (symbol < 0 || symbol >= MAX_DCODES) {
            err_set(s->err, s->errsz, "invalid distance code");
            return -1;
        }
        extra = inf_bits(s, dist_extra[symbol]);
        if (extra < 0) {
            err_set(s->err, s->errsz, "truncated distance code");
            return -1;
        }
        dist = (size_t)dist_base[symbol] + (size_t)extra;

        if (dist > s->out_pos) {
            err_set(s->err, s->errsz, "distance reaches before the start of the output");
            return -1;
        }
        if (len > s->out_len - s->out_pos) {
            err_set(s->err, s->errsz, "output larger than the declared size");
            return -1;
        }

        /* Byte by byte: runs may overlap themselves, which memcpy cannot do. */
        while (len--) {
            s->out[s->out_pos] = s->out[s->out_pos - dist];
            s->out_pos++;
        }
    }
}

/* Block compressed with the predefined code tables. */
static int inf_fixed(inflate_state *s)
{
    short lencnt[MAX_BITS + 1], lensym[FIXED_LCODES];
    short distcnt[MAX_BITS + 1], distsym[MAX_DCODES];
    huff_table lencode, distcode;
    short lengths[FIXED_LCODES];
    int symbol;

    /* Literal/length lengths as defined in RFC 1951, section 3.2.6. */
    for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
    for (; symbol < 256; symbol++)           lengths[symbol] = 9;
    for (; symbol < 280; symbol++)           lengths[symbol] = 7;
    for (; symbol < FIXED_LCODES; symbol++)  lengths[symbol] = 8;
    lencode.count = lencnt;
    lencode.symbol = lensym;
    inf_build(&lencode, lengths, FIXED_LCODES);

    for (symbol = 0; symbol < MAX_DCODES; symbol++) lengths[symbol] = 5;
    distcode.count = distcnt;
    distcode.symbol = distsym;
    inf_build(&distcode, lengths, MAX_DCODES);

    return inf_codes(s, &lencode, &distcode);
}

/* Block carrying its own code tables. */
static int inf_dynamic(inflate_state *s)
{
    short lencnt[MAX_BITS + 1], lensym[MAX_LCODES];
    short distcnt[MAX_BITS + 1], distsym[MAX_DCODES];
    huff_table lencode, distcode;
    short lengths[MAX_LCODES + MAX_DCODES];
    int nlen, ndist, ncode, index, err;

    nlen  = inf_bits(s, 5);
    ndist = inf_bits(s, 5);
    ncode = inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) {
        err_set(s->err, s->errsz, "truncated dynamic block header");
        return -1;
    }
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAX_LCODES || ndist > MAX_DCODES) {
        err_set(s->err, s->errsz, "too many codes in dynamic block");
        return -1;
    }

    /* Read the lengths of the code length alphabet, in their shuffled order. */
    for (index = 0; index < ncode; index++) {
        int len = inf_bits(s, 3);
        if (len < 0) {
            err_set(s->err, s->errsz, "truncated code length table");
            return -1;
        }
        lengths[length_order[index]] = (short)len;
    }
    for (; index < CODE_LENGTHS; index++)
        lengths[length_order[index]] = 0;

    lencode.count = lencnt;
    lencode.symbol = lensym;
    if (inf_build(&lencode, lengths, CODE_LENGTHS) != 0) {
        err_set(s->err, s->errsz, "incomplete code length table");
        return -1;
    }

    /* Decode the literal/length and distance code lengths themselves. */
    index = 0;
    while (index < nlen + ndist) {
        int symbol = inf_decode(s, &lencode);
        int repeat, value = 0;

        if (symbol < 0) {
            err_set(s->err, s->errsz, "invalid code length code");
            return -1;
        }
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }

        if (symbol == 16) {           /* repeat the previous length 3-6 times */
            if (index == 0) {
                err_set(s->err, s->errsz, "no previous code length to repeat");
                return -1;
            }
            value = lengths[index - 1];
            repeat = inf_bits(s, 2);
            if (repeat < 0) goto truncated;
            repeat += 3;
        } else if (symbol == 17) {    /* 3-10 zero lengths */
            repeat = inf_bits(s, 3);
            if (repeat < 0) goto truncated;
            repeat += 3;
        } else {                      /* 11-138 zero lengths */
            repeat = inf_bits(s, 7);
            if (repeat < 0) goto truncated;
            repeat += 11;
        }

        if (index + repeat > nlen + ndist) {
            err_set(s->err, s->errsz, "too many code lengths");
            return -1;
        }
        while (repeat--)
            lengths[index++] = (short)value;
    }

    if (lengths[END_OF_BLOCK] == 0) {
        err_set(s->err, s->errsz, "no end-of-block code");
        return -1;
    }

    /* An incomplete set is only tolerated when it holds a single code. */
    err = inf_build(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) {
        err_set(s->err, s->errsz, "invalid literal/length code table");
        return -1;
    }
    distcode.count = distcnt;
    distcode.symbol = distsym;
    err = inf_build(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) {
        err_set(s->err, s->errsz, "invalid distance code table");
        return -1;
    }

    return inf_codes(s, &lencode, &distcode);

truncated:
    err_set(s->err, s->errsz, "truncated code length repeat");
    return -1;
}

/* ---- Entry point ------------------------------------------------------- */

int inflate_raw(const void *src, size_t src_len, void *dst, size_t dst_len,
                char *err, size_t errsz)
{
    inflate_state s;
    int last, type;

    s.in = src;
    s.in_len = src_len;
    s.in_pos = 0;
    s.bit_buf = 0;
    s.bit_cnt = 0;
    s.out = dst;
    s.out_len = dst_len;
    s.out_pos = 0;
    s.err = err;
    s.errsz = errsz;

    do {
        last = inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (last < 0 || type < 0) {
            err_set(err, errsz, "truncated block header");
            return -1;
        }

        switch (type) {
            case 0:  if (inf_stored(&s) != 0)  return -1; break;
            case 1:  if (inf_fixed(&s) != 0)   return -1; break;
            case 2:  if (inf_dynamic(&s) != 0) return -1; break;
            default:
                err_set(err, errsz, "invalid block type");
                return -1;
        }
    } while (!last);

    if (s.out_pos != dst_len) {
        err_set(err, errsz, "decompressed %lu bytes, expected %lu",
                (unsigned long)s.out_pos, (unsigned long)dst_len);
        return -1;
    }
    return 0;
}
