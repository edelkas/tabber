/*
 * test_update.c - Updating tabber itself.
 *
 * The swap is the part worth testing hardest, and it is testable for real: the
 * binary being replaced is named by TABBER_EXE, so these tests point it at a
 * copy of the test executable in scratch space and let the actual code rename
 * it about. The self-check that follows a swap really does run the binary that
 * was put in place, which is why a copy of a working executable is used as the
 * "new version" and a file of rubbish as the broken one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "md5.h"
#include "platform.h"
#include "test.h"
#include "update.h"
#include "util.h"
#include "version.h"

/*
 * How long the child of the handover test takes before it leaves its mark, and
 * how long this side is willing to wait for it. The delay only has to outlast
 * the call that started it, which is a fork and a wait; the patience only has
 * to outlast a loaded machine starting a process.
 */
#define SPAWN_DELAY_MS    400
#define SPAWN_POLL_MS     100
#define SPAWN_WAIT_TRIES  100

/* A manifest naming a build for whatever platform the suite is running on. */
#define MANIFEST_FMT \
    "{\n" \
    "  \"version\": \"%s\",\n" \
    "  \"date\": \"2026-09-01T12:00:00Z\",\n" \
    "  \"notes\": \"Something changed.\",\n" \
    "  \"page\": \"https://example.invalid/releases/tag/v%s\",\n" \
    "  \"builds\": {\n" \
    "    \"" UPDATE_BUILD_KEY "\": { \"url\": \"https://example.invalid/tabber\",\n" \
    "                      \"size\": %lu, \"md5\": \"%s\" }\n" \
    "  }\n" \
    "}\n"

/* The same, cut down, with the one build under whatever key is passed in. */
#define KEYED_MANIFEST_FMT \
    "{ \"version\": \"99.0.0\", \"builds\": { \"%s\": " \
    "{ \"url\": \"https://example.invalid/tabber\", \"size\": 12, " \
    "\"md5\": \"0123456789abcdef0123456789abcdef\" } } }"

/* A release as one really is: a build for each front-end, side by side. */
#define PAIRED_MANIFEST \
    "{ \"version\": \"99.0.0\", \"builds\": {" \
    " \"" UPDATE_BUILD_KEY "\": { \"url\": \"https://example.invalid/cli\"," \
    " \"size\": 12, \"md5\": \"0123456789abcdef0123456789abcdef\" }," \
    " \"" UPDATE_BUILD_KEY UPDATE_FLAVOUR_GUI "\": " \
    "{ \"url\": \"https://example.invalid/gui\"," \
    " \"size\": 34, \"md5\": \"fedcba9876543210fedcba9876543210\" } } }"

/* ---- Versions ---------------------------------------------------------- */

/* "YYYY-MM-DD", with a month and a day that could exist. The loop stops at the
 * first character that is not what it should be, the terminator included, so
 * it never reads past the end of a string that is too short. */
static int iso_date(const char *s)
{
    int i, month, day;

    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return 0;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    if (s[10] != '\0')
        return 0;
    month = (s[5] - '0') * 10 + (s[6] - '0');
    day   = (s[8] - '0') * 10 + (s[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

static void test_versions(void)
{
    test_case("versions compare the way a human reads them");

    CHECK(update_version_compare("0.3.0", "0.2.0") > 0, "0.3.0 is above 0.2.0");
    CHECK(update_version_compare("0.2.0", "0.3.0") < 0, "and 0.2.0 below it");
    CHECK(update_version_compare("1.0.0", "1.0.0") == 0, "a version equals itself");

    /* The one that string comparison gets wrong. */
    CHECK(update_version_compare("0.10.0", "0.9.0") > 0, "0.10.0 is above 0.9.0");
    CHECK(update_version_compare("1.0.10", "1.0.9") > 0, "...and 1.0.10 above 1.0.9");

    CHECK(update_version_compare("v0.3.0", "0.3.0") == 0, "a leading v is ignored");
    CHECK(update_version_compare("1.2", "1.2.0") == 0, "a missing field reads as zero");
    CHECK(update_version_compare("2", "1.9.9") > 0, "even when most of them are missing");

    CHECK(update_version_compare("1.0.0", "1.0.0-rc1") > 0,
          "a release is above its own run-up");
    CHECK(update_version_compare("1.0.0-rc1", "1.0.0-rc1") == 0,
          "and a pre-release equals itself");

    CHECK(update_version_compare("0.0.0", TABBER_VERSION) < 0,
          "the version this was built as is above 0.0.0");

    /* The front-end's About box shows this beside the version, and nothing
     * else reads it: a typo would be seen there and nowhere else, once the
     * release carrying it had already gone out. */
    CHECK(iso_date(TABBER_DATE), "the date it went out is a real YYYY-MM-DD");
    CHECK(!iso_date("2026-13-01") && !iso_date("2026-01-32"),
          "...which a thirteenth month or a thirty-second day is not");
    CHECK(!iso_date("2026-1-1") && !iso_date("2026-01-01T00:00:00Z"),
          "...nor a short field, nor a whole timestamp");
}

/* ---- The build key ----------------------------------------------------- */

static void test_build_key(void)
{
    char err[TB_ERR_LEN];
    update_info info;
    char *text;

    test_case("the build key names the system and the architecture");

    CHECK_STR(UPDATE_BUILD_KEY, UPDATE_OS "-" UPDATE_ARCH,
              "the key is the two of them, joined");
    CHECK(strcmp(UPDATE_OS, "unknown") != 0, "the system is one we know");
    CHECK(strcmp(UPDATE_ARCH, "unknown") != 0, "so is the architecture");

    /* Windows ships x64 and x86 in the same release, so a build for the same
     * system but another architecture must not read as ours: a 32-bit tabber
     * that took the 64-bit binary would replace itself with one that cannot run.
     */
    text = str_fmt(KEYED_MANIFEST_FMT, UPDATE_OS "-etchasketch");
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_CLI, &info, err, sizeof err) == 0,
              "a release built for another architecture parses (%s)", err)) {
        CHECK(info.newer, "the newer version is known");
        CHECK(info.url == NULL, "but its build is not taken for ours");
        update_info_free(&info);
    }
    free(text);

    /* The bare system still stands in, for a release with one build per OS. */
    text = str_fmt(KEYED_MANIFEST_FMT, UPDATE_OS);
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_CLI, &info, err, sizeof err) == 0,
              "a release keyed by the bare system parses (%s)", err)) {
        CHECK(info.url != NULL, "and its build stands in for ours");
        update_info_free(&info);
    }
    free(text);
}

/* ---- Which front-end is asking ----------------------------------------- */

/*
 * A release ships a build per front-end, and taking the wrong one is the
 * quietest failure there is: a download that verifies against the size and the
 * MD5 it was promised, installs cleanly, and turns out to be the other
 * program. So each front-end asks for its own key and takes nothing else.
 */
static void test_front_end_keys(void)
{
    char err[TB_ERR_LEN];
    update_info info;
    char *text;

    test_case("each front-end is offered its own build and no other");

    CHECK_STR(UPDATE_FLAVOUR_CLI, "", "the CLI's key is the bare platform");
    CHECK(sizeof(UPDATE_BUILD_KEY UPDATE_FLAVOUR_GUI) <= UPDATE_KEY_MAX,
          "and the longest key either of them asks for fits the buffer");

    if (CHECK(update_manifest_parse(PAIRED_MANIFEST, UPDATE_FLAVOUR_CLI, &info,
                                    err, sizeof err) == 0,
              "a release with a build for each parses (%s)", err)) {
        CHECK_STR(info.url, "https://example.invalid/cli", "the CLI takes the CLI's");
        CHECK_STR(info.build, UPDATE_BUILD_KEY, "and says which key it looked under");
        update_info_free(&info);
    }
    if (CHECK(update_manifest_parse(PAIRED_MANIFEST, UPDATE_FLAVOUR_GUI, &info,
                                    err, sizeof err) == 0,
              "...and parses the same for the front-end (%s)", err)) {
        CHECK_STR(info.url, "https://example.invalid/gui",
                  "the front-end takes the front-end's");
        CHECK_STR(info.build, UPDATE_BUILD_KEY UPDATE_FLAVOUR_GUI,
                  "under the key that carries its suffix");
        CHECK_NUM(info.size, 34, "with that build's size, not the other's");
        update_info_free(&info);
    }

    /* A release from before the front-end existed has a build for this
     * platform, and it is not one the front-end may install over itself. */
    text = str_fmt(KEYED_MANIFEST_FMT, UPDATE_BUILD_KEY);
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_GUI, &info,
                                    err, sizeof err) == 0,
              "a release with only a CLI build parses (%s)", err)) {
        CHECK(info.newer, "the newer version is still known");
        CHECK(info.url == NULL, "but the CLI's build is not taken for the front-end's");
        update_info_free(&info);
    }
    free(text);

    /* The bare-system fallback carries the suffix too, both ways round. */
    text = str_fmt(KEYED_MANIFEST_FMT, UPDATE_OS UPDATE_FLAVOUR_GUI);
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_GUI, &info,
                                    err, sizeof err) == 0,
              "a front-end build keyed by the bare system parses (%s)", err)) {
        CHECK(info.url != NULL, "and stands in for the front-end's");
        update_info_free(&info);
    }
    free(text);

    text = str_fmt(KEYED_MANIFEST_FMT, UPDATE_OS);
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_GUI, &info,
                                    err, sizeof err) == 0,
              "a CLI build keyed by the bare system parses (%s)", err)) {
        CHECK(info.url == NULL, "and does not stand in for the front-end's");
        update_info_free(&info);
    }
    free(text);
}

/* ---- The manifest ------------------------------------------------------ */

static void test_manifest(void)
{
    char err[TB_ERR_LEN];
    update_info info;
    char *text;

    test_case("a release manifest is read");

    text = str_fmt(MANIFEST_FMT, "99.0.0", "99.0.0", 12UL,
                   "0123456789abcdef0123456789abcdef");
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_CLI, &info, err, sizeof err) == 0,
              "the manifest parses (%s)", err)) {
        CHECK_STR(info.version, "99.0.0", "the version is read");
        CHECK_STR(info.date, "2026-09-01T12:00:00Z", "so is the date");
        CHECK_STR(info.notes, "Something changed.", "and the notes");
        CHECK(info.newer, "99.0.0 is newer than what this was built as");
        CHECK_STR(info.url, "https://example.invalid/tabber", "the build is ours");
        CHECK_NUM(info.size, 12, "with its size");
        update_info_free(&info);
    }
    free(text);

    /* The same release, but the one already installed. */
    text = str_fmt(MANIFEST_FMT, TABBER_VERSION, TABBER_VERSION, 12UL,
                   "0123456789abcdef0123456789abcdef");
    if (CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_CLI, &info, err, sizeof err) == 0,
              "a manifest of the running version parses (%s)", err)) {
        CHECK(!info.newer, "and is not newer than itself");
        update_info_free(&info);
    }
    free(text);

    test_case("...and a manifest that cannot be trusted is refused");

    CHECK(update_manifest_parse("{ not json", UPDATE_FLAVOUR_CLI, &info, err, sizeof err) != 0,
          "something that is not JSON is refused");
    CHECK(update_manifest_parse("{ \"notes\": \"hi\" }", UPDATE_FLAVOUR_CLI,
                                &info, err, sizeof err) != 0,
          "so is one that names no version");

    /* A build with no size or no hash cannot be checked, so it is not taken. */
    text = str_fmt(MANIFEST_FMT, "99.0.0", "99.0.0", 0UL,
                   "0123456789abcdef0123456789abcdef");
    CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_CLI, &info, err, sizeof err) != 0,
          "a build with no size is refused");
    free(text);
    text = str_fmt(MANIFEST_FMT, "99.0.0", "99.0.0", 12UL, "tooshort");
    CHECK(update_manifest_parse(text, UPDATE_FLAVOUR_CLI, &info, err, sizeof err) != 0,
          "and so is one with no usable MD5");
    free(text);

    test_case("...and a release with nothing we can run is still reported");

    if (CHECK(update_manifest_parse(
                  "{ \"version\": \"99.0.0\", \"builds\": { \"vic20\": {} } }",
                  UPDATE_FLAVOUR_CLI, &info, err, sizeof err) == 0,
              "a manifest with no build for us parses (%s)", err)) {
        CHECK(info.newer, "the newer version is still known");
        CHECK(info.url == NULL, "but there is nothing to download");
        CHECK_STR(info.page, UPDATE_RELEASES_URL, "and the releases page stands in");
        update_info_free(&info);
    }
}

/* ---- The swap ---------------------------------------------------------- */

/* A world holding a stand-in for the running executable. */
typedef struct {
    char *dir;         /* scratch directory                       */
    char *exe;         /* the "running" binary, a copy of this one */
    char *staged;      /* what an update writes beside it          */
    char *aside;       /* where the old one goes                   */
    unsigned char *working;   /* bytes of a binary that really runs */
    size_t working_len;
} world;

/* Builds `info` so that `data` is exactly what it promises. */
static void describe(update_info *info, const char *version,
                     const void *data, size_t len)
{
    memset(info, 0, sizeof(*info));
    snprintf(info->version, sizeof info->version, "%s", version);
    info->url = str_dup("https://example.invalid/tabber");
    info->size = len;
    md5_hex(data, len, info->md5);
}

static int world_build(world *w, const char *name)
{
    char *self;

    memset(w, 0, sizeof(*w));
    w->dir = test_dir(name);

    /* The test binary is a real, working executable of this very version, so
     * a copy of it is both the "old" tabber and a valid "new" one. */
    self = plat_exe_path();
    w->working = self ? (unsigned char *)plat_read_file(self, &w->working_len) : NULL;
    free(self);
    if (!CHECK(w->working != NULL && w->working_len > 0,
               "the test binary can be read, to stand in for tabber"))
        return -1;

    w->exe = path_join(w->dir, "tabber_stand_in" TEST_EXE_SUFFIX);
    w->staged = str_fmt("%s%s", w->exe, UPDATE_NEW_SUFFIX);
    w->aside = str_fmt("%s%s", w->exe, UPDATE_OLD_SUFFIX);
    test_write_bytes(w->exe, w->working, w->working_len);
    plat_make_executable(w->exe);
    test_setenv(UPDATE_ENV_EXE, w->exe);
    return 0;
}

static void world_free(world *w)
{
    test_setenv(UPDATE_ENV_EXE, NULL);
    free(w->dir);
    free(w->exe);
    free(w->staged);
    free(w->aside);
    free(w->working);
}

/* Whether the stand-in is byte for byte the binary it started as. */
static int exe_is_intact(world *w)
{
    size_t len = 0;
    unsigned char *now = test_read_bytes(w->exe, &len);
    int same = now && len == w->working_len && memcmp(now, w->working, len) == 0;

    free(now);
    return same;
}

static void test_swap(void)
{
    char err[TB_ERR_LEN];
    world w;
    update_info info;
    update_plan plan;

    test_case("the new binary takes the old one's place");
    if (world_build(&w, "update_swap") != 0) { world_free(&w); return; }

    describe(&info, TABBER_VERSION, w.working, w.working_len);
    if (CHECK(update_plan_stage(&info, w.working, w.working_len, &plan,
                                err, sizeof err) == 0,
              "the download is staged (%s)", err)) {
        CHECK(plat_is_file(w.staged), "beside the binary it will replace");
        CHECK(exe_is_intact(&w), "which is untouched until it is applied");

        if (CHECK(update_plan_apply(&plan, err, sizeof err) == 0,
                  "and applied (%s)", err)) {
            CHECK(exe_is_intact(&w), "the new binary is in place");
            CHECK(plat_is_file(w.aside), "the old one was moved aside, not deleted");
            CHECK(!plat_is_file(w.staged), "and nothing is left staged");
        }
        update_plan_free(&plan);
    }
    update_info_free(&info);

    /* The leftover is only removable once the run that displaced it has gone,
     * which for a test is straight away. */
    update_sweep();
    CHECK(!plat_is_file(w.aside), "the sweep clears the displaced binary");
    CHECK(plat_is_file(w.exe), "and leaves the one in use alone");

    world_free(&w);
}

static void test_refusals(void)
{
    char err[TB_ERR_LEN];
    world w;
    update_info info;
    update_plan plan;

    test_case("a download that is not what was promised is refused");
    if (world_build(&w, "update_refuse") != 0) { world_free(&w); return; }

    /* Right hash, wrong length. */
    describe(&info, "99.0.0", w.working, w.working_len);
    info.size = w.working_len + 1;
    CHECK(update_plan_stage(&info, w.working, w.working_len, &plan,
                            err, sizeof err) != 0,
          "a download of the wrong size is refused");
    CHECK(strstr(err, "bytes") != NULL, "and the sizes are named (%s)", err);
    update_info_free(&info);

    /* Right length, wrong hash: one byte of the binary altered in flight. */
    describe(&info, "99.0.0", w.working, w.working_len);
    {
        unsigned char *tampered = xmalloc(w.working_len);

        memcpy(tampered, w.working, w.working_len);
        tampered[w.working_len / 2] ^= 0xff;
        CHECK(update_plan_stage(&info, tampered, w.working_len, &plan,
                                err, sizeof err) != 0,
              "a single altered byte is caught by the MD5");
        free(tampered);
    }
    update_info_free(&info);

    CHECK(!plat_is_file(w.staged), "neither left anything on disk");
    CHECK(exe_is_intact(&w), "and the binary in use is untouched");
    world_free(&w);
}

static void test_broken_binary_is_rolled_back(void)
{
    char err[TB_ERR_LEN];
    world w;
    update_info info;
    update_plan plan;
    static const char rubbish[] = "MZ this is not a program at all, only bytes";

    test_case("a new binary that will not run is taken straight back out");
    if (world_build(&w, "update_broken") != 0) { world_free(&w); return; }

    /* It matches the manifest perfectly; it simply is not a program. That is
     * exactly what the self-check after the swap is for. */
    describe(&info, TABBER_VERSION, rubbish, sizeof rubbish - 1);
    if (CHECK(update_plan_stage(&info, rubbish, sizeof rubbish - 1, &plan,
                                err, sizeof err) == 0,
              "it stages, being just what the manifest described (%s)", err)) {
        CHECK(update_plan_apply(&plan, err, sizeof err) != 0,
              "but applying it fails, because it does not run");
        CHECK(exe_is_intact(&w), "and the binary that did run is back in place");
        CHECK(!plan.applied, "the plan knows it was undone");
        update_plan_free(&plan);
    }
    update_info_free(&info);
    world_free(&w);
}

static void test_wrong_version_is_rolled_back(void)
{
    char err[TB_ERR_LEN];
    world w;
    update_info info;
    update_plan plan;

    test_case("...and so is one that is not the version it claimed to be");
    if (world_build(&w, "update_wrong_version") != 0) { world_free(&w); return; }

    /* A binary that runs perfectly well, described as a version it is not:
     * the manifest and the download agree with each other and both are wrong,
     * which only asking the binary itself can catch. */
    describe(&info, "99.0.0", w.working, w.working_len);
    if (CHECK(update_plan_stage(&info, w.working, w.working_len, &plan,
                                err, sizeof err) == 0,
              "it stages (%s)", err)) {
        CHECK(update_plan_apply(&plan, err, sizeof err) != 0,
              "applying it fails: it reports " TABBER_VERSION ", not 99.0.0");
        CHECK(exe_is_intact(&w), "and the binary is back as it was");
        update_plan_free(&plan);
    }
    update_info_free(&info);
    world_free(&w);
}

static void test_undo(void)
{
    char err[TB_ERR_LEN];
    world w;
    update_info info;
    update_plan plan;

    test_case("an applied update can still be taken back");
    if (world_build(&w, "update_undo") != 0) { world_free(&w); return; }

    describe(&info, TABBER_VERSION, w.working, w.working_len);
    if (CHECK(update_plan_stage(&info, w.working, w.working_len, &plan,
                                err, sizeof err) == 0,
              "staged (%s)", err) &&
        CHECK(update_plan_apply(&plan, err, sizeof err) == 0, "applied (%s)", err)) {
        update_plan_undo(&plan);
        CHECK(exe_is_intact(&w), "the old binary is back under its own name");
        CHECK(!plat_is_file(w.aside), "and nothing is left aside");
        update_plan_free(&plan);
    }
    update_info_free(&info);
    world_free(&w);
}

/* ---- Handing over ------------------------------------------------------ */

/*
 * What a windowed front-end does at the end of an update: start the binary
 * that has just replaced it and go, rather than wait for it. Waiting is what
 * this has to be told apart from, so the child takes a moment before it leaves
 * its mark — a parent that waited could not still be here to look.
 */
static void test_handover(void)
{
    char *dir = test_dir("update_spawn");
    char *mark = path_join(dir, "the-child-was-here");
    char *self = plat_exe_path();
    char *args[3];
    char delay[16];
    int started, waited = 0, i;

    test_case("the successor is started, and not waited for");
    if (!CHECK(self != NULL, "the test binary knows where it is")) {
        free(mark);
        free(dir);
        return;
    }

    snprintf(delay, sizeof delay, "%d", SPAWN_DELAY_MS);
    args[0] = (char *)TEST_TOUCH_ARG;
    args[1] = mark;
    args[2] = delay;

    started = plat_spawn_detached(self, args, 3);
    CHECK(started == 0, "a process that exists starts");
    CHECK(!plat_is_file(mark),
          "and control comes back before it has done anything");

    /* It still has to have really run: a spawn that quietly did nothing would
     * pass the check above just as well. */
    for (i = 0; i < SPAWN_WAIT_TRIES && !plat_is_file(mark); i++)
        test_sleep_ms(SPAWN_POLL_MS);
    waited = plat_is_file(mark);
    CHECK(waited, "the child runs on and leaves its mark");

    /* The one thing a caller that is about to exit can still act on. */
    CHECK(plat_spawn_detached("no such program here", NULL, 0) != 0,
          "a program that is not one is reported rather than assumed");

    free(self);
    free(mark);
    free(dir);
}

/* ---- When to look ------------------------------------------------------ */

static void test_when_to_check(void)
{
    char err[TB_ERR_LEN];
    char *dir = test_dir("update_state");
    config *cfg;

    test_case("a check is remembered, and not repeated all day");
    test_use_root(dir);

    cfg = config_load(err, sizeof err);
    if (CHECK(cfg != NULL, "the state file loads (%s)", err)) {
        CHECK(config_update_enabled(cfg), "checking is on unless it is turned off");
        CHECK(config_update_due(cfg, UPDATE_CHECK_HOURS),
              "and a check is due when none has ever run");
        CHECK(config_update_latest(cfg) == NULL, "nothing has been seen yet");
        CHECK(config_update_last_check(cfg) == NULL, "and no moment to show for it");

        config_update_checked(cfg, "99.0.0");
        CHECK(!config_update_due(cfg, UPDATE_CHECK_HOURS),
              "once one has run, the next is not due");
        CHECK(config_update_due(cfg, 0), "though it is if nothing is allowed to age");
        CHECK_STR(config_update_latest(cfg), "99.0.0", "and what it found is kept");

        /* The same stamp "is another one owed?" is answered from, handed out
         * for showing rather than for comparing: the front-end says when it
         * last looked. */
        CHECK(time_from_iso8601(config_update_last_check(cfg)) > 0,
              "and the moment it happened, as a timestamp that reads back");

        CHECK(!config_update_declined(cfg, "99.0.0"), "nothing is declined yet");
        config_update_decline(cfg, "99.0.0");
        CHECK(config_update_declined(cfg, "99.0.0"), "saying no to a version sticks");
        CHECK(!config_update_declined(cfg, "99.1.0"),
              "but only to that one: a later release asks again");

        /*
         * An update that went through is news the process that did it cannot
         * give: it hands over to the binary it installed and exits. So it is
         * left here for that one to find.
         */
        CHECK(config_update_unannounced(cfg) == NULL, "nothing is waiting to be told");
        config_update_applied(cfg, "99.0.0");
        CHECK_STR(config_update_unannounced(cfg), "99.0.0",
                  "an update that went through is left for the next run to tell");

        /* All of it has to survive the trip through the file. */
        CHECK(config_save(cfg, err, sizeof err) == 0, "the state file saves (%s)", err);
        config_free(cfg);

        cfg = config_load(err, sizeof err);
        if (CHECK(cfg != NULL, "and loads again (%s)", err)) {
            CHECK(!config_update_due(cfg, UPDATE_CHECK_HOURS), "the check is still fresh");
            CHECK(config_update_declined(cfg, "99.0.0"), "and the refusal is remembered");
            CHECK_STR(config_update_unannounced(cfg), "99.0.0",
                      "...as is the news, which is the point of writing it down");

            config_update_announced(cfg);
            CHECK(config_update_unannounced(cfg) == NULL,
                  "telling it strikes it, so it is told once and not every run");
            CHECK(config_save(cfg, err, sizeof err) == 0,
                  "the state file saves again (%s)", err);
            config_free(cfg);
        }

        cfg = config_load(err, sizeof err);
        if (CHECK(cfg != NULL, "and loads a third time (%s)", err)) {
            CHECK(config_update_unannounced(cfg) == NULL,
                  "...and stays told, once that has been written down too");
            config_free(cfg);
        }
    }

    free(dir);
}

/* ---- What the user has asked of it ------------------------------------- */

/*
 * The two standing answers the settings hold: what to do about a release when
 * one turns up, and how far apart the looks for one are. Either front-end may
 * read them and a hand may have written them, so what matters here is that a
 * file saying nothing means what it has always meant, and that a file saying
 * something no version of this wrote is read rather than refused.
 */
static void test_update_settings(void)
{
    char err[TB_ERR_LEN];
    char *dir = test_dir("update_settings");
    char *path;
    config *cfg;

    test_case("what to do about an update, and how often to look for one");
    test_use_root(dir);

    cfg = config_load(err, sizeof err);
    if (CHECK(cfg != NULL, "the state file loads (%s)", err)) {
        CHECK(config_update_policy(cfg) == UPDATE_POLICY_AUTO,
              "a tool nobody has told otherwise keeps itself up to date");
        CHECK(config_update_interval(cfg) == UPDATE_CHECK_HOURS,
              "and looks for the chance as often as it always has");

        config_set_update_policy(cfg, UPDATE_POLICY_PROMPT);
        CHECK(config_update_policy(cfg) == UPDATE_POLICY_PROMPT,
              "being asked first can be asked for");
        config_set_update_policy(cfg, UPDATE_POLICY_NONE);
        CHECK(config_update_policy(cfg) == UPDATE_POLICY_NONE,
              "...and so can hearing nothing about it");

        /* The window says "every 6 hours" or "every 10 days"; what is kept is
         * whichever of those in the hours it comes to. */
        config_update_checked(cfg, NULL);
        config_set_update_interval(cfg, 6);
        CHECK(config_update_interval(cfg) == 6, "a distance is kept as it was given");
        CHECK(!config_update_due(cfg, config_update_interval(cfg)),
              "and is what settles when the next look is owed");

        /* However it is asked for, nothing looks oftener than this. */
        config_set_update_interval(cfg, 0);
        CHECK(config_update_interval(cfg) == CONFIG_INTERVAL_MIN,
              "nought is no distance at all, and is held to the shortest there is");
        config_set_update_interval(cfg, -5);
        CHECK(config_update_interval(cfg) == CONFIG_INTERVAL_MIN,
              "...as is anything under it");

        /* Nor further apart than the front-end's own control can ask for,
         * which is as far as it can go without overflowing the hours. */
        config_set_update_interval(cfg, CONFIG_INTERVAL_MAX * 2);
        CHECK(config_update_interval(cfg) == CONFIG_INTERVAL_MAX,
              "and no further apart than there is room to say");

        config_set_update_interval(cfg, 10 * 24);
        CHECK(config_save(cfg, err, sizeof err) == 0, "the state file saves (%s)", err);
        config_free(cfg);
    }

    /* Both have to survive the trip through the file, or the window would open
     * on one answer and the tool go on acting on another. */
    cfg = config_load(err, sizeof err);
    if (CHECK(cfg != NULL, "and loads again (%s)", err)) {
        CHECK(config_update_policy(cfg) == UPDATE_POLICY_NONE,
              "what to do about a release is remembered");
        CHECK(config_update_interval(cfg) == 240, "and so is how long between looks");
        config_free(cfg);
    }

    /* A file naming things no version of this wrote is a file somebody has
     * edited: read for what can be read, not refused for the rest. */
    path = path_join(dir, CONFIG_FILENAME);
    CHECK(test_write(path, "{\"update\":{\"policy\":\"whenever\","
                           "\"interval_hours\":\"soon\"}}") == 0,
          "a hand-edited state file is written");
    cfg = config_load(err, sizeof err);
    if (CHECK(cfg != NULL, "...and still loads (%s)", err)) {
        CHECK(config_update_policy(cfg) == UPDATE_POLICY_AUTO,
              "a policy we do not know is the one we would have picked");
        CHECK(config_update_interval(cfg) == UPDATE_CHECK_HOURS,
              "and a distance that is not a number is the one we always used");
        config_free(cfg);
    }

    free(path);
    free(dir);
}

/* ---- Suite ------------------------------------------------------------- */

void suite_update(void)
{
    test_suite("update");
    test_versions();
    test_build_key();
    test_front_end_keys();
    test_manifest();
    test_swap();
    test_refusals();
    test_broken_binary_is_rolled_back();
    test_wrong_version_is_rolled_back();
    test_undo();
    test_handover();
    test_when_to_check();
    test_update_settings();
}
