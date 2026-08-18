/*
 * test_archive.c - The ZIP reader, the DEFLATE decompressor and their guards.
 *
 * Correctness of decompression is checked the way the tool itself checks it:
 * every entry's CRC-32, computed over the bytes we produced, must match the one
 * the archive recorded. That is an independent verdict on the decompressor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void suite_archive(void)
{
    test_suite("archive");
    test_open();
    test_extract();
    test_corruption();
    test_truncation();
    test_unsafe_names();
}
