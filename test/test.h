/*
 * test.h - A small harness for tabber's test suite.
 *
 * Tests are plain functions grouped into suites. Each check reports pass or
 * fail and keeps going, so one broken assumption does not hide the rest.
 *
 * Suites are tagged by tier:
 *   - offline: no network, no real game, no real state. Always run.
 *   - online:  talks to the live digest and mappack server (--online).
 *   - full:    the exhaustive sweep over every published tab (--full).
 */
#ifndef TABBER_TEST_H
#define TABBER_TEST_H

#include <stddef.h>

#include "util.h"

/* ---- Reporting --------------------------------------------------------- */

extern int test_checks;
extern int test_failures;
extern const char *test_current;

void test_suite(const char *name);
void test_case(const char *name);
int  test_report(int condition, const char *file, int line, const char *fmt, ...);

#define CHECK(cond, ...) test_report((cond), __FILE__, __LINE__, __VA_ARGS__)

/* Common shapes, so failures print what was actually seen. */
int check_str_eq(const char *got, const char *want, const char *file, int line,
                 const char *what);
int check_long_eq(long got, long want, const char *file, int line, const char *what);

#define CHECK_STR(got, want, what)  check_str_eq((got), (want), __FILE__, __LINE__, (what))
#define CHECK_NUM(got, want, what)  check_long_eq((long)(got), (long)(want), __FILE__, __LINE__, (what))

/*
 * A host and port the suite can point a server at without leaving the machine:
 * loopback, on a port nothing listens on. Connecting is refused at once, which
 * is what the health-check tests want, and no DNS or traffic is involved.
 */
#define TEST_DEAD_HOST  "127.0.0.1"
#define TEST_DEAD_PORT  9

/* ---- Scratch space ----------------------------------------------------- */

/*
 * A fresh, empty directory under the suite's own working area. Removed and
 * recreated on every call, so tests never inherit each other's leftovers.
 */
char *test_dir(const char *name);

/* Removes the whole working area. Called once when the run finishes. */
void test_cleanup(void);

/* Sets or clears an environment variable for the current process. */
void test_setenv(const char *name, const char *value);

/* Points the tool's root (config.json, digest.json, tabs/) at `dir`. */
void test_use_root(const char *dir);

/* Writes `text` to `path`, creating parent directories. Returns 0 on success. */
int test_write(const char *path, const char *text);

/* Whole file contents, or NULL. Caller frees. */
char *test_read(const char *path);

/* Whether two files hold exactly the same bytes. */
int test_files_equal(const char *a, const char *b);

/* ---- Fake game --------------------------------------------------------- */

/*
 * Builds a stand-in N++ installation under `dir`: the assets folder with every
 * level and challenge file the game supports, and a library carrying the
 * official server URI exactly once. Returns the installation directory, which
 * the caller frees, and points TABBER_ENV_GAME_DIR at it.
 */
char *test_fake_game(const char *dir);

/* The digest used by the offline suites: two tabs, one of them installable. */
extern const char *TEST_DIGEST_JSON;

/* ---- Suites ------------------------------------------------------------ */

void suite_core(void);      /* strings, buffers, paths, json, kv, md5 */
void suite_archive(void);   /* inflate, zip, integrity, unsafe paths   */
void suite_state(void);     /* config.json and server resolution       */
void suite_game(void);      /* install, uninstall, library patching    */
void suite_online(int full);/* live digest, downloads, full sweep      */

#endif /* TABBER_TEST_H */
