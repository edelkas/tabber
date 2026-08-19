/*
 * test_archive.c - The ZIP reader and writer, gzip, and their guards.
 *
 * Correctness of decompression is checked the way the tool itself checks it:
 * every entry's CRC-32, computed over the bytes we produced, must match the one
 * the archive recorded. That is an independent verdict on the decompressor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gzip.h"
#include "md5.h"
#include "test.h"
#include "util.h"
#include "zip.h"

extern const unsigned char TEST_ZIP[];
extern const size_t TEST_ZIP_LEN;

/* What the fixture holds, checked against the archive's own bookkeeping. */
#define FIXTURE_ENTRIES     4
#define FIXTURE_TOTAL_UNC   2210
#define FIXTURE_SI_LEN      2200
#define FIXTURE_SI_MD5      "c76a974fc2c7c0dfd4ffbee727deba5c"

static void test_open(void)
{
    char err[TB_ERR_LEN];
    zip_archive zip;
    const zip_entry *entry;

    test_case("reading an archive");
    CHECK(zip_open(&zip, TEST_ZIP, TEST_ZIP_LEN, err, sizeof err) == 0,
          "the fixture opens (%s)", err);
    CHECK_NUM(zip.count, FIXTURE_ENTRIES, "entry count");
    CHECK_NUM(zip_total_uncompressed(&zip), FIXTURE_TOTAL_UNC, "total uncompressed size");

    entry = zip_find(&zip, "Levels/SI.txt");
    CHECK(entry != NULL, "entries are found by path");
    if (entry) {
        CHECK_NUM(entry->uncomp_size, FIXTURE_SI_LEN, "uncompressed size of SI.txt");
        CHECK(entry->method == ZIP_METHOD_DEFLATE, "SI.txt is deflated");
        CHECK(!entry->is_dir, "a file is not a directory");
    }
    CHECK(zip_find(&zip, "levels/si.TXT") != NULL, "lookup ignores case");
    CHECK(zip_find(&zip, "Levels/Missing.txt") == NULL, "absent entries are not found");

    entry = zip_find(&zip, "Levels/");
    CHECK(entry && entry->is_dir, "the directory entry is marked as one");

    zip_close(&zip);
    CHECK_NUM(zip.count, 0, "zip_close empties the archive");
}

static void test_extract(void)
{
    char err[TB_ERR_LEN];
    char hex[MD5_HEX_LEN + 1];
    zip_archive zip;
    const zip_entry *entry;
    unsigned char *data;
    size_t i;

    test_case("decompressing entries");
    if (zip_open(&zip, TEST_ZIP, TEST_ZIP_LEN, err, sizeof err) != 0)
        return;

    /* Every entry must come out, and its CRC is the arbiter of correctness. */
    for (i = 0; i < zip.count; i++) {
        if (zip.entries[i].is_dir)
            continue;
        data = zip_read(&zip, &zip.entries[i], err, sizeof err);
        CHECK(data != NULL, "'%s' decompresses and passes its CRC (%s)",
              zip.entries[i].name, data ? "" : err);
        free(data);
    }

    entry = zip_find(&zip, "Levels/SI.txt");
    data = entry ? zip_read(&zip, entry, err, sizeof err) : NULL;
    CHECK(data != NULL, "the deflated entry decompresses");
    if (data) {
        md5_hex(data, FIXTURE_SI_LEN, hex);
        CHECK_STR(hex, FIXTURE_SI_MD5, "decompressed bytes match the original");
        CHECK_NUM(data[FIXTURE_SI_LEN], 0, "the buffer is NUL-terminated");
        free(data);
    }

    /* A deflate stored block: the shortest path through the decompressor. */
    entry = zip_find(&zip, "Levels/Scodes.txt");
    data = entry ? zip_read(&zip, entry, err, sizeof err) : NULL;
    CHECK(data != NULL, "the stored-block entry decompresses");
    if (data) {
        CHECK_STR((char *)data, "AGNT", "stored-block contents");
        free(data);
    }

    zip_close(&zip);
}

/*
 * Damage inside an entry's compressed data must always be caught, either while
 * inflating or by the CRC. Bytes elsewhere (names, timestamps) are not covered
 * by any checksum, so only the data region is swept.
 */
static void test_corruption(void)
{
    char err[TB_ERR_LEN];
    zip_archive zip;
    size_t i, spot, attempts = 0, detected = 0;

    test_case("corrupted data is rejected");
    if (zip_open(&zip, TEST_ZIP, TEST_ZIP_LEN, err, sizeof err) != 0)
        return;

    for (i = 0; i < zip.count; i++) {
        const zip_entry *entry = &zip.entries[i];
        const unsigned char *local;
        size_t data_offset;

        if (entry->is_dir || entry->comp_size < 4)
            continue;

        /* The local header carries its own name and extra lengths. */
        local = TEST_ZIP + entry->local_offset;
        data_offset = entry->local_offset + 30 +
                      ((size_t)local[26] | ((size_t)local[27] << 8)) +
                      ((size_t)local[28] | ((size_t)local[29] << 8));

        for (spot = 0; spot < entry->comp_size; spot++) {
            unsigned char *copy = xmalloc(TEST_ZIP_LEN);
            zip_archive damaged;

            memcpy(copy, TEST_ZIP, TEST_ZIP_LEN);
            copy[data_offset + spot] ^= 0xFF;
            attempts++;

            if (zip_open(&damaged, copy, TEST_ZIP_LEN, err, sizeof err) != 0) {
                detected++;
            } else {
                unsigned char *out = zip_read(&damaged, &damaged.entries[i], err, sizeof err);
                if (!out)
                    detected++;
                free(out);
                zip_close(&damaged);
            }
            free(copy);
        }
    }
    zip_close(&zip);

    CHECK(attempts > 0, "the sweep had something to corrupt");
    CHECK_NUM(detected, attempts, "every corrupted byte was caught");
}

static void test_truncation(void)
{
    char err[TB_ERR_LEN];
    zip_archive zip;
    unsigned char *copy;

    test_case("malformed archives are refused");
    CHECK(zip_open(&zip, TEST_ZIP, 10, err, sizeof err) != 0, "a stub is not an archive");
    CHECK(zip_open(&zip, "not a zip at all", 16, err, sizeof err) != 0, "plain text is not an archive");

    /* Losing the end of the file takes the central directory with it. */
    copy = xmalloc(TEST_ZIP_LEN);
    memcpy(copy, TEST_ZIP, TEST_ZIP_LEN);
    CHECK(zip_open(&zip, copy, TEST_ZIP_LEN - 40, err, sizeof err) != 0,
          "a truncated archive is refused");
    free(copy);
}

/* Paths that would write outside the destination must never be accepted. */
static void test_unsafe_names(void)
{
    static const char *unsafe[] = {
        "../evil.txt", "..\\evil.txt", "a/../../b.txt", "/etc/passwd",
        "C:\\Windows\\x.txt", "Levels/../../x", "..", "", NULL
    };
    static const char *safe[] = {
        "Levels/SI.txt", "AUTHORS", "Palettes/parametric-blue/ninja.tga",
        "a/b/c/d.txt", "..dotfile.txt", "x..y/z.txt", NULL
    };
    int i;

    test_case("unsafe entry names");
    for (i = 0; unsafe[i]; i++)
        CHECK(!zip_name_is_safe(unsafe[i]), "'%s' is rejected", unsafe[i]);
    for (i = 0; safe[i]; i++)
        CHECK(zip_name_is_safe(safe[i]), "'%s' is accepted", safe[i]);
    CHECK(!zip_name_is_safe(NULL), "a null name is rejected");
}

/* ---- Writing archives -------------------------------------------------- */

/* What the writer produces has to survive a trip through the reader. */
static void test_zip_writer(void)
{
    char err[TB_ERR_LEN];
    static const char body[] = "a savefile's worth of bytes\0with a NUL in it";
    size_t body_len = sizeof body - 1;
    unsigned char *image, *out;
    size_t image_len = 0;
    zip_archive zip;
    const zip_entry *entry;

    test_case("writing an archive");
    image = zip_create_stored("nprofile.gz", body, body_len, &image_len);
    CHECK(image != NULL && image_len > body_len, "an archive is produced");

    CHECK(zip_open(&zip, image, image_len, err, sizeof err) == 0,
          "and it parses (%s)", err);
    CHECK_NUM(zip.count, 1, "with the one entry in it");
    entry = zip_find_prefix(&zip, "nprofile");
    CHECK(entry != NULL, "found by the prefix the savefile code looks for");
    if (entry) {
        CHECK_STR(entry->name, "nprofile.gz", "under the name it was given");
        CHECK_NUM(entry->method, ZIP_METHOD_STORE, "stored, not deflated");
        CHECK_NUM(entry->uncomp_size, body_len, "with the right size");
        out = zip_read(&zip, entry, err, sizeof err);
        CHECK(out != NULL, "it reads back and passes its checksum (%s)", err);
        CHECK(out && memcmp(out, body, body_len) == 0, "byte for byte");
        free(out);
    }
    zip_close(&zip);

    /* A byte flipped anywhere in the payload must fail the checksum. */
    image[image_len / 2] ^= 0xFF;
    if (zip_open(&zip, image, image_len, err, sizeof err) == 0) {
        entry = zip_find_prefix(&zip, "nprofile");
        out = entry ? zip_read(&zip, entry, err, sizeof err) : NULL;
        CHECK(out == NULL, "a damaged copy of it is refused");
        free(out);
        zip_close(&zip);
    }
    free(image);

    /* An empty file is still a valid entry. */
    image = zip_create_stored("nprofile", "", 0, &image_len);
    CHECK(zip_open(&zip, image, image_len, err, sizeof err) == 0, "an empty entry works");
    zip_close(&zip);
    free(image);
}

/* ---- gzip -------------------------------------------------------------- */

static void test_gzip_reader(void)
{
    char err[TB_ERR_LEN];
    static const char body[] = "the uncompressed savefile, such as it is";
    size_t body_len = sizeof body - 1;
    unsigned char *gz, *out;
    size_t gz_len = 0, out_len = 0;

    test_case("reading a gzip stream");
    gz = test_gzip(body, body_len, &gz_len);
    CHECK(gz_is_gzip(gz, gz_len), "the fixture looks like gzip");
    CHECK(!gz_is_gzip(body, body_len), "plain data does not");
    CHECK(!gz_is_gzip(NULL, 0), "and neither does nothing");

    out = gz_extract(gz, gz_len, &out_len, err, sizeof err);
    CHECK(out != NULL, "it unwraps (%s)", err);
    CHECK_NUM(out_len, body_len, "to the right length");
    CHECK(out && memcmp(out, body, body_len) == 0, "with the right bytes");
    free(out);

    /* The trailer is the independent check on all of that. */
    gz[gz_len - 5] ^= 0xFF;                       /* a byte of the CRC */
    out = gz_extract(gz, gz_len, &out_len, err, sizeof err);
    CHECK(out == NULL, "a wrong checksum is refused");
    CHECK(strstr(err, "checksum") != NULL, "and named as such");
    free(out);
    gz[gz_len - 5] ^= 0xFF;

    gz[GZ_HEADER_SIZE + 3] ^= 0xFF;               /* a byte of the payload */
    out = gz_extract(gz, gz_len, &out_len, err, sizeof err);
    CHECK(out == NULL, "damaged compressed data is refused");
    free(out);
    gz[GZ_HEADER_SIZE + 3] ^= 0xFF;

    out = gz_extract(gz, gz_len - 4, &out_len, err, sizeof err);
    CHECK(out == NULL, "a truncated stream is refused");
    free(out);

    gz[2] = 99;                                   /* an unknown method */
    out = gz_extract(gz, gz_len, &out_len, err, sizeof err);
    CHECK(out == NULL, "a method that is not deflate is refused");
    free(out);

    free(gz);
    out = gz_extract("not gzip at all", 15, &out_len, err, sizeof err);
    CHECK(out == NULL, "and so is something that is not gzip");
    free(out);
}

/*
 * A gzip stream from a real encoder (zlib's, through .NET), so the reader is
 * held to more than the suite's own stored-block fixture: this one carries
 * Huffman codes and back-references.
 */
static const unsigned char REAL_GZIP[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0xf3, 0xd3,
    0xd6, 0x56, 0x28, 0x4e, 0x2c, 0x4b, 0x4d, 0xcb, 0xcc, 0x49, 0x55, 0x48,
    0xaa, 0x2c, 0x49, 0x2d, 0xd6, 0x51, 0x48, 0xce, 0xcf, 0x2d, 0x28, 0x4a,
    0x2d, 0x2e, 0x4e, 0x4d, 0x01, 0x8a, 0x28, 0x24, 0x2a, 0x14, 0xa5, 0x26,
    0xe6, 0x28, 0xa4, 0xe6, 0x25, 0xe7, 0xa7, 0xa4, 0x16, 0x59, 0x01, 0x79,
    0x05, 0xa9, 0x25, 0x99, 0x25, 0x99, 0xf9, 0x79, 0x04, 0x99, 0x7a, 0x00,
    0x03, 0x8c, 0xc7, 0x00, 0x5e, 0x00, 0x00, 0x00
};
static const char REAL_GZIP_TEXT[] =
    "N++ savefile bytes, compressed by a real encoder: repetition repetition "
    "repetition repetition.";

static void test_real_gzip(void)
{
    char err[TB_ERR_LEN];
    size_t want = sizeof REAL_GZIP_TEXT - 1;
    size_t out_len = 0;
    unsigned char *out;

    test_case("a gzip stream from a real encoder");
    out = gz_extract(REAL_GZIP, sizeof REAL_GZIP, &out_len, err, sizeof err);
    CHECK(out != NULL, "it unwraps (%s)", err);
    CHECK_NUM(out_len, want, "to the length its trailer promises");
    CHECK(out && memcmp(out, REAL_GZIP_TEXT, want) == 0, "with the right bytes");
    free(out);
}

/* ---- ZIP64 ------------------------------------------------------------- */

/* Little-endian writers, for building a doctored archive by hand. */
static void put16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)(value & 0xFF);
    p[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value & 0xFF);
    p[1] = (unsigned char)((value >> 8) & 0xFF);
    p[2] = (unsigned char)((value >> 16) & 0xFF);
    p[3] = (unsigned char)((value >> 24) & 0xFF);
}

static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/*
 * Rewrites a one-entry archive into the shape rubyzip writes: the sizes in the
 * central directory replaced by ZIP64 markers, with the real values moved into
 * an extra field. Every savefile archive the previous installers left behind
 * looks like this, whatever its size, so the reader has to cope with it.
 * `with_extra` builds the same thing without that extra field, which is a
 * broken archive and has to be refused.
 */
static unsigned char *make_zip64(const unsigned char *image, size_t len,
                                 int with_extra, size_t *out_len)
{
    byte_buf out = {0};
    size_t eocd = len - 22;                  /* no comment: it is at the end */
    size_t cd = (size_t)get32(image + eocd + 16);
    size_t cd_size = (size_t)get32(image + eocd + 12);
    size_t name_len = (size_t)image[cd + 28] | ((size_t)image[cd + 29] << 8);
    unsigned char header[46], extra[20], tail[22];
    size_t added = with_extra ? sizeof extra : 0;

    /* Everything up to the central directory is untouched. */
    buf_append(&out, image, cd);

    memcpy(header, image + cd, sizeof header);
    put32(header + 20, 0xFFFFFFFFUL);        /* compressed size   */
    put32(header + 24, 0xFFFFFFFFUL);        /* uncompressed size */
    put16(header + 30, (unsigned)added);     /* extra field length */
    buf_append(&out, header, sizeof header);
    buf_append(&out, image + cd + 46, name_len);

    if (with_extra) {
        memset(extra, 0, sizeof extra);
        put16(extra + 0, 0x0001);            /* the ZIP64 extra field   */
        put16(extra + 2, 16);                /* two 64-bit values       */
        put32(extra + 4, get32(image + cd + 24));    /* uncompressed    */
        put32(extra + 12, get32(image + cd + 20));   /* compressed      */
        buf_append(&out, extra, sizeof extra);
    }

    memcpy(tail, image + eocd, sizeof tail);
    put32(tail + 12, (unsigned long)(cd_size + added));
    buf_append(&out, tail, sizeof tail);

    return (unsigned char *)buf_finish(&out, out_len);
}

static void test_zip64_entries(void)
{
    char err[TB_ERR_LEN];
    static const char body[] = "a savefile as the old installers archived it";
    size_t body_len = sizeof body - 1;
    unsigned char *plain, *image, *out;
    size_t plain_len = 0, image_len = 0;
    zip_archive zip;
    const zip_entry *entry;

    test_case("entries whose sizes live in a ZIP64 field");
    plain = zip_create_stored("nprofile.gz", body, body_len, &plain_len);

    image = make_zip64(plain, plain_len, 1, &image_len);
    CHECK(zip_open(&zip, image, image_len, err, sizeof err) == 0,
          "such an archive opens (%s)", err);
    entry = zip_find_prefix(&zip, "nprofile");
    CHECK(entry != NULL, "and its entry is found");
    if (entry) {
        CHECK_NUM(entry->uncomp_size, body_len, "with the size out of the extra field");
        out = zip_read(&zip, entry, err, sizeof err);
        CHECK(out != NULL, "it reads back (%s)", err);
        CHECK(out && memcmp(out, body, body_len) == 0, "byte for byte");
        free(out);
    }
    zip_close(&zip);
    free(image);

    /* Marked as ZIP64 but with nothing to read: refuse, do not guess. */
    image = make_zip64(plain, plain_len, 0, &image_len);
    CHECK(zip_open(&zip, image, image_len, err, sizeof err) != 0,
          "a marker with no ZIP64 field is refused");
    CHECK(strstr(err, "ZIP64") != NULL, "and says so");
    zip_close(&zip);
    free(image);

    free(plain);
}

void suite_archive(void)
{
    test_suite("archive");
    test_open();
    test_extract();
    test_corruption();
    test_truncation();
    test_unsafe_names();
    test_zip_writer();
    test_gzip_reader();
    test_real_gzip();
    test_zip64_entries();
}
