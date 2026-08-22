/*
 * test_main.c - The harness itself, plus the shared fixtures.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "digest.h"
#include "gzip.h"
#include "loc.h"
#include "patch.h"
#include "paths.h"
#include "platform.h"
#include "save.h"
#include "test.h"
#include "util.h"
#include "zip.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* Where scratch directories are created, under the tool's own build output. */
#define TEST_WORK_DIR "testtmp"

int test_checks = 0;
int test_failures = 0;
const char *test_current = "";

static const char *current_suite = "";
static char *work_root = NULL;

/* ---- Reporting --------------------------------------------------------- */

void test_suite(const char *name)
{
    current_suite = name;
    printf("\n== %s ==\n", name);
}

void test_case(const char *name)
{
    test_current = name;
    printf("  %s\n", name);
}

int test_report(int condition, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    test_checks++;
    if (condition)
        return 1;

    test_failures++;
    fprintf(stdout, "    FAIL [%s] ", current_suite);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "  (%s:%d)\n", file, line);
    return 0;
}

int check_str_eq(const char *got, const char *want, const char *file, int line,
                 const char *what)
{
    if (got && want && strcmp(got, want) == 0)
        return test_report(1, file, line, "%s", what);
    return test_report(0, file, line, "%s: got '%s', wanted '%s'",
                       what, got ? got : "(null)", want ? want : "(null)");
}

int check_long_eq(long got, long want, const char *file, int line, const char *what)
{
    if (got == want)
        return test_report(1, file, line, "%s", what);
    return test_report(0, file, line, "%s: got %ld, wanted %ld", what, got, want);
}

/* ---- Scratch space ----------------------------------------------------- */

static const char *test_work_root(void)
{
    if (!work_root) {
        char *exe = plat_exe_dir();
        work_root = path_join(exe ? exe : ".", TEST_WORK_DIR);
        free(exe);
        plat_mkdir_p(work_root);
    }
    return work_root;
}

char *test_dir(const char *name)
{
    char *dir = path_join(test_work_root(), name);

    plat_remove_tree(dir);   /* start from nothing every time */
    plat_mkdir_p(dir);
    return dir;
}

void test_cleanup(void)
{
    if (work_root) {
        plat_remove_tree(work_root);
        free(work_root);
        work_root = NULL;
    }
}

void test_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    /* The tool reads the process block through GetEnvironmentVariableW. */
    int wide_name_len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    wchar_t *wide_name = xmalloc((size_t)wide_name_len * sizeof(wchar_t));
    wchar_t *wide_value = NULL;

    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, wide_name_len);
    if (value) {
        int len = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
        wide_value = xmalloc((size_t)len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, value, -1, wide_value, len);
    }
    SetEnvironmentVariableW(wide_name, wide_value);
    free(wide_name);
    free(wide_value);
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

void test_use_root(const char *dir)
{
    test_setenv(TABBER_ENV_HOME, dir);
}

int test_write(const char *path, const char *text)
{
    char *parent = path_dirname(path);
    int rc;

    plat_mkdir_p(parent);
    free(parent);
    rc = plat_write_file(path, text, strlen(text));
    return rc;
}

char *test_read(const char *path)
{
    return plat_read_file(path, NULL);
}

int test_write_bytes(const char *path, const void *data, size_t len)
{
    char *parent = path_dirname(path);
    int rc;

    plat_mkdir_p(parent);
    free(parent);
    rc = plat_write_file(path, data, len);
    return rc;
}

unsigned char *test_read_bytes(const char *path, size_t *len_out)
{
    return (unsigned char *)plat_read_file(path, len_out);
}

int test_write_zip(const char *path, const char *entry, const void *data, size_t len)
{
    size_t zip_len = 0;
    unsigned char *image = zip_create_stored(entry, data, len, &zip_len);
    int rc = test_write_bytes(path, image, zip_len);

    free(image);
    return rc;
}

/*
 * A gzip stream whose DEFLATE payload is a single stored block. That keeps the
 * fixture honest — a real header, a real trailer, a real deflate stream — with
 * no compressor in the suite.
 */
unsigned char *test_gzip(const void *data, size_t len, size_t *len_out)
{
    static const unsigned char header[GZ_HEADER_SIZE] = {
        GZ_MAGIC_0, GZ_MAGIC_1, GZ_METHOD_DEFLATE, 0, 0, 0, 0, 0, 0, 0xFF
    };
    byte_buf out = {0};
    unsigned long crc = crc32_bytes(data, len);
    unsigned char block[5], trailer[8];

    buf_append(&out, header, sizeof header);

    block[0] = 0x01;                                  /* final, stored     */
    block[1] = (unsigned char)(len & 0xFF);           /* LEN               */
    block[2] = (unsigned char)((len >> 8) & 0xFF);
    block[3] = (unsigned char)(~block[1]);            /* NLEN, its inverse */
    block[4] = (unsigned char)(~block[2]);
    buf_append(&out, block, sizeof block);
    buf_append(&out, data, len);

    trailer[0] = (unsigned char)(crc & 0xFF);
    trailer[1] = (unsigned char)((crc >> 8) & 0xFF);
    trailer[2] = (unsigned char)((crc >> 16) & 0xFF);
    trailer[3] = (unsigned char)((crc >> 24) & 0xFF);
    trailer[4] = (unsigned char)(len & 0xFF);
    trailer[5] = (unsigned char)((len >> 8) & 0xFF);
    trailer[6] = (unsigned char)((len >> 16) & 0xFF);
    trailer[7] = (unsigned char)((len >> 24) & 0xFF);
    buf_append(&out, trailer, sizeof trailer);

    return (unsigned char *)buf_finish(&out, len_out);
}

char *test_fake_personal(const char *dir, const char *save_name,
                         const void *save, size_t len)
{
    char *personal = path_join(dir, "personal");

    plat_mkdir_p(personal);
    if (save_name) {
        char *path = path_join(personal, save_name);
        test_write_bytes(path, save, len);
        free(path);
    }
    test_setenv(TABBER_ENV_PERSONAL_DIR, personal);
    return personal;
}

void test_use_fresh_save(const char *path)
{
    test_setenv(TABBER_ENV_FRESH_SAVE, path);
}

int test_files_equal(const char *a, const char *b)
{
    size_t alen = 0, blen = 0;
    char *da = plat_read_file(a, &alen);
    char *db = plat_read_file(b, &blen);
    int same = da && db && alen == blen && memcmp(da, db, alen) == 0;

    free(da);
    free(db);
    return same;
}

/* ---- Fake game --------------------------------------------------------- */

/* Every file name the game reads, matching the digest's global lists. */
static const char *game_level_files[] = {
    "SI.txt", "S.txt", "SL.txt", "SS.txt", "S2.txt", "SS2.txt", "SSS.txt",
    "SSS2.txt", "CI.txt", "C.txt", "C2.txt", "CL.txt", "CL2.txt", "CT.txt",
    "RI.txt", "R.txt", "R2.txt", "RL.txt", "RL2.txt", "RT.txt", NULL
};
static const char *game_challenge_files[] = {
    "Scodes.txt", "SScodes.txt", "S2codes.txt", "SS2codes.txt",
    "SSScodes.txt", "SSS2codes.txt", NULL
};

char *test_fake_game(const char *dir)
{
    char *install = path_join(dir, "N++");
    char *levels = NULL;
    npp_paths paths = {0};
    char *library;
    byte_buf blob = {0};
    int i;

    /* The assets folder is what marks a directory as an installation. */
    {
        char *assets = path_join(install, NPP_ASSETS_SUBDIR);
        levels = path_join(assets, DIGEST_DEFAULT_LEVELS_DIR);
        plat_mkdir_p(levels);
        free(assets);
    }

    for (i = 0; game_level_files[i]; i++) {
        char *path = path_join(levels, game_level_files[i]);
        char *body = str_fmt("original %s", game_level_files[i]);
        test_write(path, body);
        free(path);
        free(body);
    }
    for (i = 0; game_challenge_files[i]; i++) {
        char *path = path_join(levels, game_challenge_files[i]);
        char *body = str_fmt("original %s", game_challenge_files[i]);
        test_write(path, body);
        free(path);
        free(body);
    }
    free(levels);

    /* The game's string table, two languages wide, holding the strings a tab
     * replaces among a couple it leaves alone. */
    {
        char *assets = path_join(install, NPP_ASSETS_SUBDIR);
        char *loc = path_join(assets, LOC_FILE_NAME);

        test_write(loc, TEST_LOC_TABLE);
        free(loc);
        free(assets);
    }

    /*
     * A stand-in library: filler, the official URI exactly once followed by a
     * NUL, then more filler. The filler is a single repeated character so it
     * cannot accidentally contain a URI we search for.
     */
    paths.install_dir = install;
    library = lib_path(&paths);
    {
        char *parent = path_dirname(library);
        plat_mkdir_p(parent);
        free(parent);
    }
    for (i = 0; i < 1024; i++)
        buf_append(&blob, "A", 1);
    buf_append(&blob, LIB_OFFICIAL_URI, sizeof(LIB_OFFICIAL_URI) - 1);
    buf_append(&blob, "\0", 1);
    for (i = 0; i < 1024; i++)
        buf_append(&blob, "B", 1);
    plat_write_file(library, blob.data, blob.len);
    buf_free(&blob);
    free(library);

    test_setenv(TABBER_ENV_GAME_DIR, install);
    return install;
}

/*
 * The game's string table as the fake installation carries it: the header, the
 * three strings a custom tab replaces, and two it does not.
 */
const char *TEST_LOC_TABLE =
    "LOC_ID|english|spanish\n"
    "EPISODE|Episode|Episodio\n"
    "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG|Friends Highscores|Records de amigos\n"
    "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT|Friends|Amigos\n"
    "PLAYER_PRESS_ANY|Press Any Key|Pulsa una tecla\n"
    "LEVEL|Level|Nivel\n";

/*
 * A digest with the same shape as the published one: the global config lists,
 * and two tabs. "tst" is the one the game suites install; "non" ships a file
 * the game does not read, to exercise the skip path.
 */
const char *TEST_DIGEST_JSON =
"{\n"
"  \"config\": {\n"
"    \"levels_dir\": \"Levels\",\n"
"    \"palettes_dir\": \"Palettes\",\n"
"    \"level_files\": [\"SI.txt\", \"S.txt\", \"SL.txt\", \"SS.txt\", \"S2.txt\",\n"
"                     \"SS2.txt\", \"SSS.txt\", \"SSS2.txt\", \"CI.txt\", \"C.txt\",\n"
"                     \"C2.txt\", \"CL.txt\", \"CL2.txt\", \"CT.txt\", \"RI.txt\",\n"
"                     \"R.txt\", \"R2.txt\", \"RL.txt\", \"RL2.txt\", \"RT.txt\"],\n"
"    \"challenge_files\": [\"Scodes.txt\", \"SScodes.txt\", \"S2codes.txt\",\n"
"                         \"SS2codes.txt\", \"SSScodes.txt\", \"SSS2codes.txt\"]\n"
"  },\n"
"  \"tabs\": [\n"
"    {\n"
"      \"attributes\": { \"id\": 0, \"name\": \"Test Tab\", \"code\": \"tst\",\n"
"                        \"authors\": \"Nobody\", \"date\": \"2026-01-02T00:00:00.000Z\",\n"
"                        \"version\": 1, \"enabled\": true },\n"
"      \"download\": { \"link\": \"https://example.invalid/tst.zip\", \"size\": 1, \"md5\": \"0\" },\n"
"      \"disk\": { \"size\": 2, \"level_files\": [\"SI.txt\"],\n"
"                  \"challenge_files\": [\"Scodes.txt\"],\n"
"                  \"palettes\": [\"test palette\"] },\n"
"      \"properties\": {}, \"stats\": {}\n"
"    },\n"
"    {\n"
"      \"attributes\": { \"id\": 1, \"name\": \"Other Tab\", \"code\": \"oth\",\n"
"                        \"authors\": \"Someone\", \"date\": \"2026-02-03T00:00:00.000Z\",\n"
"                        \"version\": 1, \"enabled\": true },\n"
"      \"download\": { \"link\": \"https://example.invalid/oth.zip\", \"size\": 1, \"md5\": \"0\" },\n"
"      \"disk\": { \"size\": 2, \"level_files\": [\"S.txt\"],\n"
"                  \"challenge_files\": [], \"palettes\": [] },\n"
"      \"properties\": {}, \"stats\": {}\n"
"    }\n"
"  ],\n"
"  \"server\": { \"host\": \"" TEST_DEAD_HOST "\", \"port\": 9 },\n"
"  \"signature\": { \"md5\": \"deadbeef\", \"date\": \"2026-01-01T00:00:00Z\" }\n"
"}\n";

/* ---- Entry point ------------------------------------------------------- */

int main(int argc, char **argv)
{
    int online = 0, full = 0, i;

    plat_init();

    /*
     * Point the personal folder at scratch space before anything runs: no
     * lookup can then reach the real one, whatever a test forgets to set.
     */
    {
        char *guard = test_dir("no_personal_folder");
        test_setenv(TABBER_ENV_PERSONAL_DIR, guard);
        free(guard);
    }

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--online")) {
            online = 1;
        } else if (!strcmp(argv[i], "--full")) {
            online = full = 1;
        } else if (!strcmp(argv[i], "--help")) {
            printf("Usage: test_tabber [--online] [--full]\n"
                   "  --online  also run the tests that need the network\n"
                   "  --full    also sweep every tab in the live digest (slow)\n");
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    suite_core();
    suite_archive();
    suite_state();
    suite_save();
    suite_palettes();
    suite_loc();
    suite_keys();
    suite_game();
    if (online)
        suite_online(full);
    else
        printf("\n== online (skipped, pass --online to include) ==\n");

    /* Leave no environment behind for whatever runs next. */
    test_setenv(TABBER_ENV_HOME, NULL);
    test_setenv(TABBER_ENV_GAME_DIR, NULL);
    test_setenv(TABBER_ENV_PERSONAL_DIR, NULL);
    test_setenv(TABBER_ENV_FRESH_SAVE, NULL);
    test_cleanup();

    printf("\n%d checks, %d failure(s): %s\n",
           test_checks, test_failures, test_failures ? "FAILED" : "all green");
    return test_failures ? 1 : 0;
}
