#include <stdlib.h>
#include <string.h>

#include "deflate.h"
#include "util.h"

/* ---- The three alphabets ----------------------------------------------- */

#define LITLEN_SYMS   288    /* 286 are used; the fixed code defines 288    */
#define DIST_SYMS     30
#define CL_SYMS       19     /* the alphabet the code lengths are sent in   */
#define MAX_BITS      15     /* longest code the format allows              */
#define CL_MAX_BITS   7      /* ...and for the code-length alphabet         */
#define END_BLOCK     256    /* the symbol that closes a block              */

/* Block types, as they go into the three-bit block header. */
#define BLOCK_STORED  0
#define BLOCK_FIXED   1
#define BLOCK_DYNAMIC 2

/* Marks an empty slot in the hash chains. */
#define NO_POS        0xFFFFFFFFu

/* Longest distance we will code. One short of the window, so a candidate can
 * never alias the position it is being compared against in the chain array. */
#define MAX_DIST      (DEFLATE_WINDOW - 1)

/* Length codes 257..285: the length each one starts at, and its extra bits. */
static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* Distance codes 0..29, the same way. */
static const unsigned short dist_base[DIST_SYMS] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[DIST_SYMS] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* The order the code-length code's own lengths are transmitted in. */
static const unsigned char cl_order[CL_SYMS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ---- Bit writer -------------------------------------------------------- */

/* DEFLATE packs bits into bytes from the least significant end upwards. */
typedef struct {
    byte_buf out;
    unsigned long acc;
    int nbits;
} bit_writer;

static void bw_bits(bit_writer *w, unsigned value, int count)
{
    w->acc |= (unsigned long)value << w->nbits;
    w->nbits += count;
    while (w->nbits >= 8) {
        unsigned char byte = (unsigned char)(w->acc & 0xFF);

        buf_append(&w->out, &byte, 1);
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

/* Pads out to the next byte boundary, as a stored block needs. */
static void bw_align(bit_writer *w)
{
    if (w->nbits > 0)
        bw_bits(w, 0, 8 - w->nbits);
}

/*
 * Huffman codes go out most significant bit first, which is the other way
 * round from everything else, so they are stored reversed and written as-is.
 */
static unsigned rev_bits(unsigned code, int count)
{
    unsigned out = 0;
    int i;

    for (i = 0; i < count; i++) {
        out = (out << 1) | (code & 1);
        code >>= 1;
    }
    return out;
}

/* ---- Huffman codes ----------------------------------------------------- */

typedef struct {
    unsigned char len[LITLEN_SYMS];
    unsigned short code[LITLEN_SYMS];   /* reversed, ready to write */
} code_table;

/* Orders symbols by frequency, least frequent first, ties by symbol. */
static void sort_by_freq(const unsigned *freq, int *order, int count)
{
    int i, j;

    /* Insertion sort: at most 288 entries, and nearly ordered on the retries. */
    for (i = 1; i < count; i++) {
        int sym = order[i];

        for (j = i; j > 0; j--) {
            int prev = order[j - 1];

            if (freq[prev] < freq[sym] || (freq[prev] == freq[sym] && prev < sym))
                break;
            order[j] = prev;
        }
        order[j] = sym;
    }
}

/*
 * Bit lengths for a Huffman code over `freq`, none longer than max_bits.
 * Symbols with no occurrences get length 0.
 *
 * The tree is built the usual way, from the two cheapest nodes upwards. If it
 * comes out deeper than the format allows, the frequencies are halved and it
 * is built again: that keeps their order but pulls the extremes together, and
 * once they are all equal the tree cannot be deeper than log2 of the alphabet.
 * Capping this way costs a fraction of a percent on the rare block it happens
 * to, and cannot produce a code the format could not express.
 */
static void huff_build(const unsigned *freq_in, int n, int max_bits, unsigned char *len)
{
    unsigned freq[LITLEN_SYMS];
    unsigned node_freq[2 * LITLEN_SYMS];
    int parent[2 * LITLEN_SYMS];
    int order[LITLEN_SYMS];
    int used = 0, i;

    memset(len, 0, (size_t)n);
    memcpy(freq, freq_in, (size_t)n * sizeof(*freq));
    for (i = 0; i < n; i++) {
        if (freq[i])
            order[used++] = i;
    }
    if (used == 0)
        return;
    if (used == 1) {
        len[order[0]] = 1;      /* an incomplete code; the callers avoid it */
        return;
    }

    for (;;) {
        int leaf = 0, internal = used, next = used, deepest = 0;

        sort_by_freq(freq, order, used);
        for (i = 0; i < used; i++) {
            node_freq[i] = freq[order[i]];
            parent[i] = -1;
        }

        /* Merge the two cheapest nodes until one tree is left, taking them
         * from whichever of the two queues — leaves not yet used, internal
         * nodes already made — has the smaller one at its front. Both stay in
         * order on their own, so no searching is needed. */
        while (next < 2 * used - 1) {
            int pick[2], k;

            for (k = 0; k < 2; k++) {
                if (leaf < used &&
                    (internal >= next || node_freq[leaf] <= node_freq[internal]))
                    pick[k] = leaf++;
                else
                    pick[k] = internal++;
            }
            node_freq[next] = node_freq[pick[0]] + node_freq[pick[1]];
            parent[next] = -1;
            parent[pick[0]] = parent[pick[1]] = next;
            next++;
        }

        /* A leaf's distance from the root is its code length. */
        for (i = 0; i < used; i++) {
            int depth = 0, node = i;

            while (parent[node] >= 0) {
                node = parent[node];
                depth++;
            }
            len[order[i]] = (unsigned char)depth;
            if (depth > deepest)
                deepest = depth;
        }
        if (deepest <= max_bits)
            return;

        for (i = 0; i < used; i++)
            freq[order[i]] = (freq[order[i]] + 1) / 2;
    }
}

/* Canonical codes for a set of lengths, stored reversed for writing. */
static void huff_assign(const unsigned char *len, int n, unsigned short *code)
{
    unsigned count[MAX_BITS + 1], next[MAX_BITS + 1];
    unsigned value = 0;
    int bits, i;

    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;

    for (bits = 1; bits <= MAX_BITS; bits++) {
        value = (value + count[bits - 1]) << 1;
        next[bits] = value;
    }
    for (i = 0; i < n; i++) {
        if (len[i])
            code[i] = (unsigned short)rev_bits(next[len[i]]++, len[i]);
        else
            code[i] = 0;
    }
}

/* Builds a table, making sure the code is complete even for tiny alphabets. */
static void build_table(unsigned *freq, int n, int max_bits, code_table *table)
{
    int used = 0, i;

    for (i = 0; i < n; i++)
        used += freq[i] != 0;

    /* A code of a single symbol cannot be written down: give the alphabet a
     * second symbol so both come out one bit long. */
    if (used < 2) {
        freq[0] = freq[0] ? freq[0] : 1;
        freq[1] = freq[1] ? freq[1] : 1;
    }

    huff_build(freq, n, max_bits, table->len);
    huff_assign(table->len, n, table->code);
}

/* The code the format defines, used when a block's own would cost more. */
static void build_fixed(code_table *lit, code_table *dist)
{
    int i;

    for (i = 0; i < LITLEN_SYMS; i++)
        lit->len[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    huff_assign(lit->len, LITLEN_SYMS, lit->code);

    for (i = 0; i < DIST_SYMS; i++)
        dist->len[i] = 5;
    huff_assign(dist->len, DIST_SYMS, dist->code);
}

/* ---- Symbols ----------------------------------------------------------- */

/* One literal, or one match: dist is 0 for a literal. */
typedef struct {
    unsigned short literal;
    unsigned short dist;
} symbol;

/* Which length code covers `length`, and which distance code covers `dist`. */
static int length_code(unsigned length)
{
    int i;

    for (i = 28; i > 0; i--) {
        if (length >= len_base[i])
            break;
    }
    return i;
}

static int distance_code(unsigned dist)
{
    int i;

    for (i = DIST_SYMS - 1; i > 0; i--) {
        if (dist >= dist_base[i])
            break;
    }
    return i;
}

/* ---- Code lengths, as they are transmitted ----------------------------- */

/* One entry of the run-length encoded code-length sequence. */
typedef struct {
    unsigned char sym;
    unsigned char extra_bits;
    unsigned char extra;
} cl_item;

/*
 * Run-length encodes the code lengths of both tables into `items`: repeats of
 * a length become symbol 16, runs of zeros become 17 or 18. Returns how many
 * entries were produced and counts each symbol in `freq`.
 */
static size_t encode_lengths(const unsigned char *lengths, size_t n,
                             cl_item *items, unsigned *freq)
{
    size_t at = 0, count = 0;

    while (at < n) {
        unsigned char value = lengths[at];
        size_t run = 1;

        while (at + run < n && lengths[at + run] == value)
            run++;
        at += run;

        if (value == 0) {
            while (run >= 11) {
                size_t take = run > 138 ? 138 : run;

                items[count].sym = 18;
                items[count].extra_bits = 7;
                items[count].extra = (unsigned char)(take - 11);
                freq[18]++;
                count++;
                run -= take;
            }
            while (run >= 3) {
                size_t take = run > 10 ? 10 : run;

                items[count].sym = 17;
                items[count].extra_bits = 3;
                items[count].extra = (unsigned char)(take - 3);
                freq[17]++;
                count++;
                run -= take;
            }
        } else {
            /* Symbol 16 repeats what came before it, so send the value once. */
            items[count].sym = value;
            items[count].extra_bits = 0;
            items[count].extra = 0;
            freq[value]++;
            count++;
            run--;

            while (run >= 3) {
                size_t take = run > 6 ? 6 : run;

                items[count].sym = 16;
                items[count].extra_bits = 2;
                items[count].extra = (unsigned char)(take - 3);
                freq[16]++;
                count++;
                run -= take;
            }
        }

        while (run-- > 0) {
            items[count].sym = value;
            items[count].extra_bits = 0;
            items[count].extra = 0;
            freq[value]++;
            count++;
        }
    }
    return count;
}

/* ---- Blocks ------------------------------------------------------------ */

/* What one block's worth of symbols costs under a given pair of tables. */
static size_t symbols_cost(const symbol *syms, size_t count,
                           const code_table *lit, const code_table *dist)
{
    size_t bits = 0, i;

    for (i = 0; i < count; i++) {
        if (syms[i].dist == 0) {
            bits += lit->len[syms[i].literal];
        } else {
            int lc = length_code(syms[i].literal);
            int dc = distance_code(syms[i].dist);

            bits += (size_t)lit->len[257 + lc] + len_extra[lc];
            bits += (size_t)dist->len[dc] + dist_extra[dc];
        }
    }
    return bits + lit->len[END_BLOCK];
}

/* Writes the symbols themselves, and the end-of-block marker after them. */
static void write_symbols(bit_writer *w, const symbol *syms, size_t count,
                          const code_table *lit, const code_table *dist)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (syms[i].dist == 0) {
            bw_bits(w, lit->code[syms[i].literal], lit->len[syms[i].literal]);
        } else {
            int lc = length_code(syms[i].literal);
            int dc = distance_code(syms[i].dist);

            bw_bits(w, lit->code[257 + lc], lit->len[257 + lc]);
            if (len_extra[lc])
                bw_bits(w, syms[i].literal - len_base[lc], len_extra[lc]);
            bw_bits(w, dist->code[dc], dist->len[dc]);
            if (dist_extra[dc])
                bw_bits(w, syms[i].dist - dist_base[dc], dist_extra[dc]);
        }
    }
    bw_bits(w, lit->code[END_BLOCK], lit->len[END_BLOCK]);
}

/* The whole encoder's state for one call. */
typedef struct {
    const unsigned char *data;
    size_t len;
    bit_writer w;
    unsigned *head;                    /* hash -> latest position at it     */
    unsigned *prev;                    /* position -> the one before it     */
    symbol *syms;
    size_t sym_count;
    unsigned lit_freq[LITLEN_SYMS];
    unsigned dist_freq[DIST_SYMS];
} deflater;

/*
 * Emits one block in whichever form is smallest. `start`/`end` are the input
 * it covers, which a stored block writes out verbatim.
 */
static void emit_block(deflater *d, size_t start, size_t end, int final)
{
    code_table lit, dist, fixed_lit, fixed_dist;
    code_table cl;
    cl_item items[2 * LITLEN_SYMS];
    unsigned char lengths[LITLEN_SYMS + DIST_SYMS];
    unsigned cl_freq[LITLEN_SYMS];     /* only CL_SYMS used; sized for reuse */
    size_t item_count, stored_bits, fixed_bits, dynamic_bits, header_bits, i;
    size_t span = end - start;
    int hlit = 257, hdist = 1, hclen = CL_SYMS, type;

    d->lit_freq[END_BLOCK]++;          /* every block ends with one */

    /* The block's own codes... */
    build_table(d->lit_freq, LITLEN_SYMS, MAX_BITS, &lit);
    build_table(d->dist_freq, DIST_SYMS, MAX_BITS, &dist);

    /* Only the codes actually in use are transmitted. */
    for (i = LITLEN_SYMS; i > 257; i--) {
        if (lit.len[i - 1]) {
            hlit = (int)i;
            break;
        }
    }
    for (i = DIST_SYMS; i > 1; i--) {
        if (dist.len[i - 1]) {
            hdist = (int)i;
            break;
        }
    }

    memcpy(lengths, lit.len, (size_t)hlit);
    memcpy(lengths + hlit, dist.len, (size_t)hdist);
    memset(cl_freq, 0, sizeof(unsigned) * CL_SYMS);
    item_count = encode_lengths(lengths, (size_t)(hlit + hdist), items, cl_freq);
    build_table(cl_freq, CL_SYMS, CL_MAX_BITS, &cl);

    while (hclen > 4 && cl.len[cl_order[hclen - 1]] == 0)
        hclen--;

    header_bits = 3 + 5 + 5 + 4 + 3 * (size_t)hclen;
    for (i = 0; i < item_count; i++)
        header_bits += (size_t)cl.len[items[i].sym] + items[i].extra_bits;

    /* ...against the two alternatives. */
    dynamic_bits = header_bits + symbols_cost(d->syms, d->sym_count, &lit, &dist);
    build_fixed(&fixed_lit, &fixed_dist);
    fixed_bits = 3 + symbols_cost(d->syms, d->sym_count, &fixed_lit, &fixed_dist);
    stored_bits = span <= DEFLATE_BLOCK_BYTES ? 3 + 7 + 32 + 8 * span : (size_t)-1;

    type = BLOCK_DYNAMIC;
    if (fixed_bits < dynamic_bits)
        type = BLOCK_FIXED;
    if (stored_bits < dynamic_bits && stored_bits < fixed_bits)
        type = BLOCK_STORED;

    bw_bits(&d->w, final ? 1 : 0, 1);
    bw_bits(&d->w, (unsigned)type, 2);

    if (type == BLOCK_STORED) {
        bw_align(&d->w);
        bw_bits(&d->w, (unsigned)(span & 0xFF), 8);
        bw_bits(&d->w, (unsigned)((span >> 8) & 0xFF), 8);
        bw_bits(&d->w, (unsigned)(~span & 0xFF), 8);
        bw_bits(&d->w, (unsigned)((~span >> 8) & 0xFF), 8);
        buf_append(&d->w.out, d->data + start, span);
        return;
    }

    if (type == BLOCK_FIXED) {
        write_symbols(&d->w, d->syms, d->sym_count, &fixed_lit, &fixed_dist);
        return;
    }

    bw_bits(&d->w, (unsigned)(hlit - 257), 5);
    bw_bits(&d->w, (unsigned)(hdist - 1), 5);
    bw_bits(&d->w, (unsigned)(hclen - 4), 4);
    for (i = 0; i < (size_t)hclen; i++)
        bw_bits(&d->w, cl.len[cl_order[i]], 3);
    for (i = 0; i < item_count; i++) {
        bw_bits(&d->w, cl.code[items[i].sym], cl.len[items[i].sym]);
        if (items[i].extra_bits)
            bw_bits(&d->w, items[i].extra, items[i].extra_bits);
    }
    write_symbols(&d->w, d->syms, d->sym_count, &lit, &dist);
}

/* ---- Matching ---------------------------------------------------------- */

static unsigned hash_at(const unsigned char *p)
{
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2])
           & (DEFLATE_HASH_SIZE - 1);
}

/* Records `pos` as the newest position with its hash. */
static void insert_pos(deflater *d, size_t pos)
{
    unsigned hash;

    if (pos + DEFLATE_MIN_MATCH > d->len)
        return;
    hash = hash_at(d->data + pos);
    d->prev[pos & (DEFLATE_WINDOW - 1)] = d->head[hash];
    d->head[hash] = (unsigned)pos;
}

/*
 * Longest match for the bytes at `pos` among the recent positions with the
 * same hash. Returns its length, 0 when there is none worth taking, and puts
 * the distance in *dist_out.
 */
static unsigned find_match(deflater *d, size_t pos, unsigned *dist_out)
{
    const unsigned char *data = d->data;
    size_t left = d->len - pos;
    unsigned limit = left > DEFLATE_MAX_MATCH ? DEFLATE_MAX_MATCH : (unsigned)left;
    unsigned best = 0, best_dist = 0, chain = DEFLATE_MAX_CHAIN;
    unsigned cand;

    *dist_out = 0;
    if (limit < DEFLATE_MIN_MATCH)
        return 0;

    cand = d->head[hash_at(data + pos)];
    while (cand != NO_POS && chain-- > 0) {
        size_t dist = pos - cand;
        unsigned length = 0;

        if (dist == 0 || dist > MAX_DIST)
            break;                      /* out of the window: so is the rest */

        /* The byte past the best match so far has to match, or this candidate
         * cannot beat it. */
        if (best == 0 || data[cand + best] == data[pos + best]) {
            while (length < limit && data[cand + length] == data[pos + length])
                length++;
            if (length > best) {
                best = length;
                best_dist = (unsigned)dist;
                if (best >= DEFLATE_NICE_MATCH || best >= limit)
                    break;
            }
        }

        cand = d->prev[cand & (DEFLATE_WINDOW - 1)];
    }

    if (best < DEFLATE_MIN_MATCH)
        return 0;
    *dist_out = best_dist;
    return best;
}

/* ---- The compressor ---------------------------------------------------- */

unsigned char *deflate_raw(const void *data, size_t len, size_t *out_len)
{
    deflater d;
    size_t pos = 0;

    memset(&d, 0, sizeof d);
    d.data = data;
    d.len = len;
    d.head = xmalloc(DEFLATE_HASH_SIZE * sizeof(*d.head));
    d.prev = xmalloc(DEFLATE_WINDOW * sizeof(*d.prev));
    d.syms = xmalloc(DEFLATE_BLOCK_SYMS * sizeof(*d.syms));
    memset(d.head, 0xFF, DEFLATE_HASH_SIZE * sizeof(*d.head));
    memset(d.prev, 0xFF, DEFLATE_WINDOW * sizeof(*d.prev));

    /* One pass per block: gather symbols, then write the block out. Empty
     * input still gets a block, since a stream needs its final marker. */
    do {
        size_t start = pos;

        d.sym_count = 0;
        memset(d.lit_freq, 0, sizeof d.lit_freq);
        memset(d.dist_freq, 0, sizeof d.dist_freq);

        while (pos < len && d.sym_count < DEFLATE_BLOCK_SYMS &&
               pos - start <= DEFLATE_BLOCK_BYTES - DEFLATE_MAX_MATCH) {
            unsigned dist = 0;
            unsigned length = find_match(&d, pos, &dist);
            symbol *sym = &d.syms[d.sym_count++];

            if (length >= DEFLATE_MIN_MATCH) {
                unsigned i;

                sym->literal = (unsigned short)length;
                sym->dist = (unsigned short)dist;
                d.lit_freq[257 + length_code(length)]++;
                d.dist_freq[distance_code(dist)]++;
                /* Every position inside the match still has to go into the
                 * chains, or later matches would miss them. */
                for (i = 0; i < length; i++)
                    insert_pos(&d, pos + i);
                pos += length;
            } else {
                sym->literal = d.data[pos];
                sym->dist = 0;
                d.lit_freq[d.data[pos]]++;
                insert_pos(&d, pos);
                pos++;
            }
        }

        emit_block(&d, start, pos, pos >= len);
    } while (pos < len);

    bw_align(&d.w);

    free(d.head);
    free(d.prev);
    free(d.syms);
    return (unsigned char *)buf_finish(&d.w.out, out_len);
}
