/*
 * update.h - Updating tabber itself.
 *
 * Releases are published on GitHub, each carrying one plain executable per
 * platform per front-end and a manifest describing them all. The manifest
 * lives at a fixed URL that always redirects to the newest release, so
 * checking for an update is one small download:
 *
 *   {
 *     "version": "0.3.0",
 *     "date":    "2026-09-01T12:00:00Z",
 *     "notes":   "What changed, in a line or three.",
 *     "page":    "https://github.com/edelkas/tabber/releases/tag/v0.3.0",
 *     "builds":  {
 *       "windows-x64":     { "url": "https://.../tabber-cli-windows-x64.exe",
 *                            "size": 371712, "md5": "..." },
 *       "windows-x64-gui": { "url": "https://.../tabber-gui-windows-x64.exe",
 *                            "size": 1143296, "md5": "..." }
 *     }
 *   }
 *
 * The build is looked up by platform and architecture ("windows-x64"), falling
 * back to the platform alone ("windows") for a release that ships one build
 * per system. A manifest that names no build we can run is not an update.
 *
 * Which of the two front-ends is asking is part of that key: they are separate
 * programs and each replaces itself, so the caller says which it is (see
 * UPDATE_FLAVOUR_CLI and UPDATE_FLAVOUR_GUI). Asking is not optional, because
 * the failure it prevents is a quiet one — a program that verifies a download
 * perfectly, installs it, and finds it has become the other front-end.
 *
 * Applying one rests on a single fact: a running executable cannot be written
 * to or deleted, but it can be *renamed*, on Windows as much as on Unix. So
 * the new binary is written beside the old one, the old one is moved aside,
 * and the new one takes its name — no helper process, no batch file. The
 * displaced binary is deleted on the next run, once nothing has it open.
 *
 * Nothing is swapped until the download has been checked against the size and
 * the MD5 the manifest declares, and nothing is kept unless the new binary
 * then proves it runs: it is asked its own version (see UPDATE_SELF_CHECK_ARG)
 * and has to agree with the manifest. If it does not, the old one goes back.
 */
#ifndef TABBER_UPDATE_H
#define TABBER_UPDATE_H

#include <stddef.h>

#include "md5.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where the manifest lives. Overridable at build time
 * (-DUPDATE_MANIFEST_URL='"..."') to test against a staging release.
 */
#ifndef UPDATE_MANIFEST_URL
#define UPDATE_MANIFEST_URL "https://github.com/edelkas/tabber/releases/latest/download/manifest.json"
#endif

/* Where to send someone who would rather do it by hand. */
#ifndef UPDATE_RELEASES_URL
#define UPDATE_RELEASES_URL "https://github.com/edelkas/tabber/releases"
#endif

/* Keys of the manifest. */
#define UJK_VERSION      "version"
#define UJK_DATE         "date"
#define UJK_NOTES        "notes"
#define UJK_PAGE         "page"
#define UJK_BUILDS       "builds"
#define UJK_URL          "url"
#define UJK_SIZE         "size"
#define UJK_MD5          "md5"

/* Which build of a release this program is. */
#if defined(_WIN32)
#  define UPDATE_OS   "windows"
#elif defined(__APPLE__)
#  define UPDATE_OS   "macos"
#elif defined(__linux__)
#  define UPDATE_OS   "linux"
#else
#  define UPDATE_OS   "unknown"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define UPDATE_ARCH "x64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define UPDATE_ARCH "arm64"
#elif defined(__i386__) || defined(_M_IX86)
#  define UPDATE_ARCH "x86"
#else
#  define UPDATE_ARCH "unknown"
#endif

#define UPDATE_BUILD_KEY UPDATE_OS "-" UPDATE_ARCH

/*
 * ...and which front-end of it. A release ships a CLI and a GUI build for each
 * platform, keyed apart by this: the CLI's build key is the bare platform, the
 * GUI's carries the suffix. Passed to update_check and update_manifest_parse
 * so that a program can only be offered the build it is.
 */
#define UPDATE_FLAVOUR_CLI   ""
#define UPDATE_FLAVOUR_GUI   "-gui"

/* Room for the longest of those keys, with the suffix and the terminator. */
#define UPDATE_KEY_MAX       32

/* How long a check stays good, so ordinary commands are not held up by one. */
#define UPDATE_CHECK_HOURS   24

/* Names taken beside the executable while an update is being applied. */
#define UPDATE_NEW_SUFFIX    ".new"
#define UPDATE_OLD_SUFFIX    ".old"

/*
 * The new binary is asked to confirm what it is before the old one is let go:
 * `tabber --self-check <version>` exits 0 only from that very version.
 */
#define UPDATE_SELF_CHECK_ARG "--self-check"

/* Set for the restarted process, so an update cannot set another going. */
#define UPDATE_ENV_GUARD     "TABBER_UPDATED"

/*
 * Stands in for the running executable, so the swap can be exercised on a copy
 * rather than on tabber itself. For the tests; nobody else has a use for it.
 */
#define UPDATE_ENV_EXE       "TABBER_EXE"

/* Longest version string handled, room enough for "10.20.30-rc1" and more. */
#define UPDATE_VERSION_MAX   32

/*
 * Compares two versions the way a human reads them: numerically, field by
 * field, with a leading "v" ignored and a missing field counting as zero, so
 * "0.10" is above "0.9" and "1.0" above "1.0-rc1". Returns <0, 0 or >0.
 */
int update_version_compare(const char *a, const char *b);

/* ---- What a release says about itself ---------------------------------- */

typedef struct {
    char version[UPDATE_VERSION_MAX];
    char date[TB_TIMESTAMP_LEN + 1];
    char *notes;                  /* may be NULL                            */
    char *page;                   /* release page, may be NULL              */
    char *url;                    /* the build for this platform, NULL when
                                     the release ships none we can run      */
    char build[UPDATE_KEY_MAX];   /* the key it was looked for under        */
    char md5[MD5_HEX_LEN + 1];
    size_t size;
    int newer;                    /* newer than the running version         */
} update_info;

/*
 * Reads a manifest, taking the build for this platform in `flavour`'s
 * front-end. Returns 0 and fills `info` (release it with update_info_free),
 * or -1 with a reason in `err`.
 */
int update_manifest_parse(const char *text, const char *flavour,
                          update_info *info, char *err, size_t errsz);

/* Downloads the manifest and reads it. Returns 0, or -1 with a reason. */
int update_check(const char *flavour, update_info *info, char *err, size_t errsz);

void update_info_free(update_info *info);

/* ---- The swap ---------------------------------------------------------- */

typedef struct {
    char *exe;                    /* the running executable          */
    char *staged;                 /* the new one, written beside it  */
    char *aside;                  /* where the old one is moved to   */
    char version[UPDATE_VERSION_MAX];
    size_t bytes;                 /* size of the new binary          */
    int applied;
} update_plan;

/*
 * Downloads the build `info` names, checks its size and MD5, and writes it
 * beside the running executable. Nothing is replaced yet. Returns 0, or -1
 * with a reason in `err` and nothing left on disk.
 */
int update_plan_build(const update_info *info, update_plan *plan, char *err, size_t errsz);

/*
 * The same from bytes already in hand, which is how the tests reach this
 * without a network. `data` still has to match what `info` declares.
 */
int update_plan_stage(const update_info *info, const void *data, size_t len,
                      update_plan *plan, char *err, size_t errsz);

/*
 * Moves the running executable aside and puts the new one in its place, then
 * makes it prove it runs. Returns 0, or -1 with a reason in `err` and the old
 * binary back where it was.
 */
int update_plan_apply(update_plan *plan, char *err, size_t errsz);

/* Puts the old binary back, for a failure after a successful apply. */
void update_plan_undo(update_plan *plan);

void update_plan_free(update_plan *plan);

/*
 * Deletes the binary an earlier update moved aside, which cannot be removed
 * while it is still running. Called once at startup; silent either way.
 */
void update_sweep(void);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_UPDATE_H */
