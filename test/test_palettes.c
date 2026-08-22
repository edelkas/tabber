/*
 * test_palettes.c - Copying a tab's palettes into the game, and taking them out.
 *
 * Palettes are folders, and the interesting part is not the copying but the
 * bookkeeping around it: names that are already taken, names taken by a
 * palette baked into the library that no amount of looking at the game's
 * folder would reveal, and the ceiling of 256 the game stops reading at.
 *
 * Everything runs against a stand-in installation in scratch space, so no real
 * game folder is ever listed, let alone written to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "digest.h"
#include "palettes.h"
#include "paths.h"
#include "platform.h"
#include "tabs.h"
#include "test.h"
#include "util.h"

#define TAB_CODE     "pal"
#define PAL_A        "test alpha"
#define PAL_B        "test beta"
#define SWATCH       "background.tga"
#define SWATCH_TWO   "menu.tga"

/* A digest naming whatever palettes a test wants the tab to bundle. */
static const char *DIGEST_FMT =
"{\n"
"  \"config\": { \"levels_dir\": \"Levels\", \"palettes_dir\": \"Palettes\",\n"
"                \"level_files\": [\"SI.txt\"], \"challenge_files\": [] },\n"
"  \"tabs\": [\n"
"    { \"attributes\": { \"id\": 0, \"name\": \"Palette Tab\", \"code\": \"" TAB_CODE "\",\n"
"                        \"authors\": \"Nobody\", \"date\": \"2026-01-02T00:00:00.000Z\",\n"
"                        \"version\": 1, \"enabled\": true },\n"
"      \"download\": { \"link\": \"https://example.invalid/pal.zip\", \"size\": 1, \"md5\": \"0\" },\n"
"      \"disk\": { \"size\": 2, \"level_files\": [\"SI.txt\"], \"challenge_files\": [],\n"
"                  \"palettes\": [%s] },\n"
"      \"properties\": {}, \"stats\": {} }\n"
"  ]\n"
"}\n";

/* A tool root, a tab store and a stand-in installation directory. */
typedef struct {
    char *root;
    char *install;   /* the fake installation directory      */
    char *game;      /* the game's palettes folder           */
    char *store;     /* the tab's palettes folder in the store */
    digest *dig;
    const npp_tab *tab;
    npp_paths paths;
} world;

/* Writes one palette folder: two swatches, so file counts mean something. */
static void make_palette(const char *dir, const char *name, const char *body)
{
    char *folder = path_join(dir, name);
    char *path = path_join(folder, SWATCH);

    test_write(path, body);
    free(path);
    path = path_join(folder, SWATCH_TWO);
    test_write(path, body);
    free(path);
    free(folder);
}

/* The body of a palette's first swatch, or NULL when it is not there. */
static char *palette_body(const char *dir, const char *name)
{
    char *folder = path_join(dir, name);
    char *path = path_join(folder, SWATCH);
    char *text = test_read(path);

    free(path);
    free(folder);
    return text;
}

static int palette_there(const char *dir, const char *name)
{
    char *folder = path_join(dir, name);
    int there = plat_is_dir(folder);

    free(folder);
    return there;
}

/*
 * Builds the world. `bundled` is the JSON array body naming what the tab
 * bundles; a folder is created in the store for each name in `names`.
 */
static void world_build(world *w, const char *dir_name, const char *bundled,
                        const char *const *names, size_t count)
{
    char err[TB_ERR_LEN];
    char *digest_path, *text, *tab_root, *assets;
    size_t i;

    memset(w, 0, sizeof(*w));
    w->root = test_dir(dir_name);
    test_use_root(w->root);

    text = str_fmt(DIGEST_FMT, bundled);
    digest_path = digest_cache_path();
    test_write(digest_path, text);
    free(digest_path);
    free(text);

    /* The installation: only its palettes folder matters here, and even that
     * is left absent until a test or an install creates it. */
    w->install = path_join(w->root, "game");
    assets = path_join(w->install, NPP_ASSETS_SUBDIR);
    w->game = path_join(assets, DIGEST_DEFAULT_PALETTES_DIR);
    plat_mkdir_p(assets);
    free(assets);
    w->paths.install_dir = str_dup(w->install);

    tab_root = tab_dir_path(TAB_CODE);
    w->store = path_join(tab_root, DIGEST_DEFAULT_PALETTES_DIR);
    free(tab_root);
    for (i = 0; i < count; i++)
        make_palette(w->store, names[i], "the tab's own colours");

    w->dig = digest_load(err, sizeof err);
    CHECK(w->dig != NULL, "the digest fixture loads (%s)", w->dig ? "" : err);
    w->tab = w->dig ? digest_find(w->dig, TAB_CODE) : NULL;
    CHECK(w->tab != NULL, "and names the tab");
}

static void world_free(world *w)
{
    digest_free(w->dig);
    npp_paths_free(&w->paths);
    free(w->root);
    free(w->install);
    free(w->game);
    free(w->store);
}

/* Runs a whole install of the tab's palettes, reporting what happened. */
static int install_palettes(world *w, palette_collision mode, palette_report *report,
                            char *err, size_t errsz)
{
    palette_plan plan = {0};
    int rc;

    if (palettes_plan_build(w->dig, w->tab, &w->paths, mode, &plan, err, errsz) != 0)
        return -1;
    rc = palettes_plan_apply(&plan, report, err, errsz);
    if (rc == 0)
        palettes_plan_commit(&plan);
    palettes_plan_free(&plan);
    return rc;
}

/* The report line for a palette, or NULL. */
static const palette_item *line_for(const palette_report *report, const char *name)
{
    size_t i;

    for (i = 0; i < report->count; i++) {
        if (str_ieq(report->items[i].name, name))
            return &report->items[i];
    }
    return NULL;
}

static void check_outcome(const palette_report *report, const char *name,
                          palette_outcome want, const char *target)
{
    const palette_item *line = line_for(report, name);

    if (!line) {
        CHECK(0, "'%s' has a line in the report", name);
        return;
    }
    CHECK(line->outcome == want, "'%s': %s, expected %s", name,
          palette_outcome_text(line->outcome), palette_outcome_text(want));
    if (target)
        CHECK_STR(line->target, target, "the folder it took");
}

/* ---- The baked names --------------------------------------------------- */

static void test_baked_names(void)
{
    size_t i, j, cut = 0;

    test_case("the baked palettes");

    for (i = 0; i < PALETTE_BAKED_COUNT; i++) {
        size_t len = strlen(palette_baked_names[i]);
        size_t suffix = sizeof(PALETTE_CUT_SUFFIX) - 1;

        if (len > suffix && !strcmp(palette_baked_names[i] + len - suffix, PALETTE_CUT_SUFFIX))
            cut++;
        for (j = 0; j < i; j++) {
            if (str_ieq(palette_baked_names[i], palette_baked_names[j])) {
                CHECK(0, "'%s' is in the table twice", palette_baked_names[i]);
                break;
            }
        }
    }
    CHECK_NUM(cut, 4, "four palettes were cut before release");

    /* A few spot checks across the two halves of the list. */
    CHECK(palette_is_baked("vasquez"), "the default palette is baked");
    CHECK(palette_is_baked("VASQUEZ"), "and the name is matched either way");
    CHECK(palette_is_baked("BASIC"), "the first one is there");
    CHECK(palette_is_baked("powder"), "and so is the last");
    CHECK(palette_is_baked("TR-808"), "punctuation and all");
    CHECK(!palette_is_baked("nova cosmic"), "a custom palette is not baked");
    CHECK(!palette_is_baked(""), "nor is nothing at all");

    /*
     * The cut four were baked with " CUT" appended, so their plain names are
     * free: that is exactly how a custom palette brings them back.
     */
    CHECK(palette_is_baked("line CUT"), "'line' was baked as 'line CUT'");
    CHECK(!palette_is_baked("line"), "which leaves 'line' free to use");
    CHECK(palette_is_baked("papier CUT") && !palette_is_baked("papier"),
          "the same for 'papier'");
    CHECK(palette_is_baked("tycho CUT") && !palette_is_baked("tycho"),
          "...and 'tycho'");
    CHECK(palette_is_baked("epaper invert CUT") && !palette_is_baked("epaper invert"),
          "...and 'epaper invert'");
    CHECK(palette_is_baked("epaper"), "while 'epaper' itself shipped");
}

static void test_collision_modes(void)
{
    palette_collision mode;

    test_case("the collision modes");

    CHECK(palette_collision_parse("skip", &mode) == 0 && mode == PALETTE_SKIP, "skip");
    CHECK(palette_collision_parse("REPLACE", &mode) == 0 && mode == PALETTE_REPLACE,
          "replace, whatever the case");
    CHECK(palette_collision_parse("suffix", &mode) == 0 && mode == PALETTE_SUFFIX, "suffix");
    CHECK(palette_collision_parse("clobber", &mode) != 0, "and nothing else");
    CHECK(palette_collision_parse(NULL, &mode) != 0, "not even nothing at all");
    CHECK_STR(palette_collision_name(PALETTE_SUFFIX), "suffix", "they have names");
}

/* ---- Installing -------------------------------------------------------- */

static void test_fresh_install(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A, PAL_B };
    world w;
    palette_report report;
    char *body;

    test_case("palettes into a game that has none");
    world_build(&w, "pal_fresh", "\"" PAL_A "\", \"" PAL_B "\"", names, 2);
    if (!w.tab) { world_free(&w); return; }

    /* The game ships without the folder on Windows: it has to be created. */
    CHECK(!plat_is_dir(w.game), "the game has no palettes folder to start with");

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the palettes go in (%s)", err);
    CHECK(plat_is_dir(w.game), "the folder was created");
    CHECK_NUM(report.count, 2, "both are reported");
    CHECK_NUM(report.installed, 2, "and both went in");
    CHECK_NUM(report.existing, 0, "the game had none before");
    CHECK_NUM(report.total, PALETTE_BAKED_COUNT + 2, "the game now sees 125");
    check_outcome(&report, PAL_A, PAL_INSTALLED, PAL_A);
    check_outcome(&report, PAL_B, PAL_INSTALLED, PAL_B);

    body = palette_body(w.game, PAL_A);
    CHECK_STR(body, "the tab's own colours", "the swatches came across");
    free(body);
    if (line_for(&report, PAL_A))
        CHECK_NUM(line_for(&report, PAL_A)->files, 2, "every file in the folder");

    palette_report_free(&report);
    world_free(&w);
}

static void test_no_palettes(void)
{
    char err[TB_ERR_LEN];
    world w;
    palette_report report;

    test_case("a tab that bundles none");
    world_build(&w, "pal_none", "", NULL, 0);
    if (!w.tab) { world_free(&w); return; }

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the empty plan applies (%s)", err);
    CHECK_NUM(report.count, 0, "with nothing to report");
    CHECK(!plat_is_dir(w.game), "and the game's folder is not created for nothing");

    palette_report_free(&report);
    world_free(&w);
}

static void test_collisions(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A };
    world w;
    palette_report report;
    char *body;

    /* --- skip: the palette that is there stays --- */
    test_case("a name already taken, skipped");
    world_build(&w, "pal_skip", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    CHECK_NUM(report.installed, 0, "nothing went in");
    check_outcome(&report, PAL_A, PAL_SKIPPED_PRESENT, NULL);
    body = palette_body(w.game, PAL_A);
    CHECK_STR(body, "the user's own colours", "and theirs is untouched");
    free(body);
    palette_report_free(&report);
    world_free(&w);

    /* --- replace: ours goes over theirs --- */
    test_case("a name already taken, replaced");
    world_build(&w, "pal_replace", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(install_palettes(&w, PALETTE_REPLACE, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    CHECK_NUM(report.installed, 1, "ours went in");
    check_outcome(&report, PAL_A, PAL_REPLACED, PAL_A);
    body = palette_body(w.game, PAL_A);
    CHECK_STR(body, "the tab's own colours", "over theirs");
    free(body);
    CHECK_NUM(report.total, PALETTE_BAKED_COUNT + 1,
              "and the count did not grow, one folder took another's place");
    {
        /* The copy kept while the install could still be undone is gone. */
        char *stash = str_fmt("%s%s", PAL_A, PALETTE_STASH_SUFFIX);
        CHECK(!palette_there(w.game, stash), "no leftovers of the old one");
        free(stash);
    }
    palette_report_free(&report);
    world_free(&w);

    /* --- suffix: ours goes in beside theirs --- */
    test_case("a name already taken, suffixed");
    world_build(&w, "pal_suffix", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(install_palettes(&w, PALETTE_SUFFIX, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, PAL_A, PAL_RENAMED, PAL_A " 2");
    body = palette_body(w.game, PAL_A);
    CHECK_STR(body, "the user's own colours", "theirs is still theirs");
    free(body);
    body = palette_body(w.game, PAL_A " 2");
    CHECK_STR(body, "the tab's own colours", "and ours sits beside it");
    free(body);
    palette_report_free(&report);
    world_free(&w);

    /* --- suffix again: " 2" is taken as well --- */
    test_case("a suffixed name already taken too");
    world_build(&w, "pal_suffix2", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "theirs");
    make_palette(w.game, PAL_A " 2", "theirs as well");

    CHECK(install_palettes(&w, PALETTE_SUFFIX, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, PAL_A, PAL_RENAMED, PAL_A " 3");
    palette_report_free(&report);
    world_free(&w);
}

static void test_case_insensitive(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A };
    world w;
    palette_report report;
    char *body;

    test_case("names differing only in case are the same name");
    world_build(&w, "pal_case", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, "TEST ALPHA", "the user's own colours");

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, PAL_A, PAL_SKIPPED_PRESENT, NULL);
    body = palette_body(w.game, "TEST ALPHA");
    CHECK_STR(body, "the user's own colours", "the palette there is untouched");
    free(body);

    palette_report_free(&report);
    world_free(&w);
}

static void test_baked_collision(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { "vasquez", "line" };
    world w;
    palette_report report;

    /*
     * A folder named after a baked palette is ignored by the game, so copying
     * it in would only spend one of the 256 slots on something invisible. The
     * cut palettes are the exception: "line" is free because the library holds
     * it as "line CUT".
     */
    test_case("a name the library bakes");
    world_build(&w, "pal_baked", "\"vasquez\", \"line\"", names, 2);
    if (!w.tab) { world_free(&w); return; }

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, "vasquez", PAL_SKIPPED_BAKED, NULL);
    check_outcome(&report, "line", PAL_INSTALLED, "line");
    CHECK(!palette_there(w.game, "vasquez"), "nothing was copied for the baked one");
    CHECK(palette_there(w.game, "line"), "the cut one is brought back");
    palette_report_free(&report);
    world_free(&w);

    /* Replacing cannot win against the library either, so it is not tried. */
    test_case("a baked name cannot be replaced");
    world_build(&w, "pal_baked_replace", "\"vasquez\"", names, 1);
    if (!w.tab) { world_free(&w); return; }

    CHECK(install_palettes(&w, PALETTE_REPLACE, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, "vasquez", PAL_SKIPPED_BAKED, NULL);
    CHECK(!palette_there(w.game, "vasquez"), "and nothing is written");
    palette_report_free(&report);
    world_free(&w);

    /* Suffixing does work: the folder no longer answers to a baked name. */
    test_case("a baked name can be suffixed out of the way");
    world_build(&w, "pal_baked_suffix", "\"vasquez\"", names, 1);
    if (!w.tab) { world_free(&w); return; }

    CHECK(install_palettes(&w, PALETTE_SUFFIX, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, "vasquez", PAL_RENAMED, "vasquez 2");
    CHECK(palette_there(w.game, "vasquez 2"), "and it is on disk under that name");
    palette_report_free(&report);
    world_free(&w);
}

static void test_bundled_twice(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A };
    world w;
    palette_report report;
    size_t i, seen = 0;

    test_case("the same palette bundled twice");
    world_build(&w, "pal_twice", "\"" PAL_A "\", \"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    CHECK_NUM(report.count, 2, "both entries are reported");
    CHECK_NUM(report.installed, 1, "but only one folder is made");
    for (i = 0; i < report.count; i++)
        seen += report.items[i].outcome == PAL_SKIPPED_DUP;
    CHECK_NUM(seen, 1, "the second one says why it was left out");

    palette_report_free(&report);
    world_free(&w);
}

/* ---- The ceiling ------------------------------------------------------- */

/* Fills the game's folder with `count` palettes of its own. */
static void crowd_folder(const char *dir, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        char name[32];
        snprintf(name, sizeof name, "filler %03u", (unsigned)i);
        make_palette(dir, name, "not ours");
    }
}

static void test_limit(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A, PAL_B };
    world w;
    palette_report report;

    /* One slot left, two palettes wanting it. */
    test_case("the game's palette limit");
    world_build(&w, "pal_limit", "\"" PAL_A "\", \"" PAL_B "\"", names, 2);
    if (!w.tab) { world_free(&w); return; }
    crowd_folder(w.game, PALETTE_MAX_CUSTOM - 1);

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    CHECK_NUM(report.existing, PALETTE_MAX_CUSTOM - 1, "the folder was nearly full");
    CHECK_NUM(report.installed, 1, "one palette fits");
    check_outcome(&report, PAL_A, PAL_INSTALLED, PAL_A);
    check_outcome(&report, PAL_B, PAL_SKIPPED_FULL, NULL);
    CHECK(!palette_there(w.game, PAL_B), "and the one that does not fit is not copied");
    CHECK_NUM(report.total, PALETTE_LIMIT, "the game is now reading its very last one");
    palette_report_free(&report);
    world_free(&w);

    /*
     * A palette that replaces one takes no new slot, so a full folder must not
     * stop it: the name check comes first, and settles the matter.
     */
    test_case("a full folder still takes a replacement");
    world_build(&w, "pal_limit_replace", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    crowd_folder(w.game, PALETTE_MAX_CUSTOM - 1);
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(install_palettes(&w, PALETTE_REPLACE, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, PAL_A, PAL_REPLACED, PAL_A);
    {
        char *body = palette_body(w.game, PAL_A);
        CHECK_STR(body, "the tab's own colours", "and ours is the one in place");
        free(body);
    }
    palette_report_free(&report);
    world_free(&w);

    /* Skipping is likewise decided before the limit is ever consulted. */
    test_case("a full folder skips by name, not by room");
    world_build(&w, "pal_limit_skip", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    crowd_folder(w.game, PALETTE_MAX_CUSTOM);
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    check_outcome(&report, PAL_A, PAL_SKIPPED_PRESENT, NULL);
    palette_report_free(&report);
    world_free(&w);
}

/*
 * Copies of the baked palettes sitting in the folder are common — the Linux
 * build ships them, and they are handy to have around to edit from — and the
 * game skips them before parsing, so they take up none of the 256 slots.
 * Counting them would tally those palettes twice and turn tabs away for a
 * shortage of room that does not exist.
 */
static void test_baked_folders_are_free(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A, PAL_B };
    world w;
    palette_report report;

    test_case("folders named after baked palettes take no room");
    world_build(&w, "pal_baked_room", "\"" PAL_A "\", \"" PAL_B "\"", names, 2);
    if (!w.tab) { world_free(&w); return; }

    /* Two slots free, and three folders the game will never look at. */
    crowd_folder(w.game, PALETTE_MAX_CUSTOM - 2);
    make_palette(w.game, "vasquez", "a copy of the baked one");
    make_palette(w.game, "PICO-8", "and another");
    make_palette(w.game, "line CUT", "and a cut one, baked under this name");

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    CHECK_NUM(report.existing, PALETTE_MAX_CUSTOM - 2,
              "the three baked-named folders are not counted");
    CHECK_NUM(report.installed, 2, "so both palettes still fit");
    check_outcome(&report, PAL_A, PAL_INSTALLED, PAL_A);
    check_outcome(&report, PAL_B, PAL_INSTALLED, PAL_B);
    CHECK_NUM(report.total, PALETTE_LIMIT, "and the game is filled exactly to the brim");
    palette_report_free(&report);
    world_free(&w);

    /* The same thing said plainly, with nothing else in the way. */
    test_case("only the folders the game reads are counted");
    world_build(&w, "pal_baked_count", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, "vasquez", "a copy of the baked one");
    make_palette(w.game, "somebody's own", "a real custom palette");

    CHECK(install_palettes(&w, PALETTE_SKIP, &report, err, sizeof err) == 0,
          "the install runs (%s)", err);
    CHECK_NUM(report.existing, 1, "one of the two folders counts");
    CHECK_NUM(report.total, PALETTE_BAKED_COUNT + 2,
              "the baked palette is not tallied twice");
    palette_report_free(&report);
    world_free(&w);
}

/* ---- Refusals and undo -------------------------------------------------- */

static void test_missing_source(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A };
    world w;
    palette_plan plan = {0};

    test_case("a palette the digest promises but the store lacks");
    world_build(&w, "pal_missing", "\"" PAL_A "\", \"" PAL_B "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }

    err[0] = '\0';
    CHECK(palettes_plan_build(w.dig, w.tab, &w.paths, PALETTE_SKIP, &plan,
                              err, sizeof err) != 0,
          "the plan is refused");
    CHECK(err[0] != '\0', "with a reason: %s", err);
    CHECK(!plat_is_dir(w.game), "and the game's folder is untouched");
    palettes_plan_free(&plan);

    world_free(&w);
}

static void test_undo(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A, PAL_B };
    world w;
    palette_plan plan = {0};
    palette_report report;
    char *body;

    test_case("undoing an install");
    world_build(&w, "pal_undo", "\"" PAL_A "\", \"" PAL_B "\"", names, 2);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(palettes_plan_build(w.dig, w.tab, &w.paths, PALETTE_REPLACE, &plan,
                              err, sizeof err) == 0, "the plan is built (%s)", err);
    CHECK(palettes_plan_apply(&plan, &report, err, sizeof err) == 0,
          "and applied (%s)", err);
    body = palette_body(w.game, PAL_A);
    CHECK_STR(body, "the tab's own colours", "ours is in place");
    free(body);

    /* Something later in the install failed: everything goes back. */
    palettes_plan_undo(&plan);
    body = palette_body(w.game, PAL_A);
    CHECK_STR(body, "the user's own colours", "theirs comes back after the undo");
    free(body);
    CHECK(!palette_there(w.game, PAL_B), "and ours is gone again");
    {
        char *stash = str_fmt("%s%s", PAL_A, PALETTE_STASH_SUFFIX);
        CHECK(!palette_there(w.game, stash), "with nothing left aside");
        free(stash);
    }

    palette_report_free(&report);
    palettes_plan_free(&plan);
    world_free(&w);
}

/* ---- Removing ---------------------------------------------------------- */

static void test_removal(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A };
    world w;
    palette_report report;
    str_list list = {0};
    char *body;

    test_case("removing the palettes a tab brought");
    world_build(&w, "pal_remove", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the tab's own colours");
    make_palette(w.game, PAL_B, "somebody else's");

    str_list_push(&list, str_dup(PAL_A));
    str_list_push(&list, str_dup("never installed"));

    CHECK(palettes_remove(w.dig, &w.paths, &list, 0, &report, err, sizeof err) == 0,
          "the removal runs (%s)", err);
    CHECK_NUM(report.removed, 1, "one folder was deleted");
    check_outcome(&report, PAL_A, PAL_REMOVED, NULL);
    check_outcome(&report, "never installed", PAL_ABSENT, NULL);
    CHECK(!palette_there(w.game, PAL_A), "ours is gone");
    body = palette_body(w.game, PAL_B);
    CHECK_STR(body, "somebody else's", "and a palette that was not ours is left alone");
    free(body);

    palette_report_free(&report);
    str_list_free(&list);
    world_free(&w);
}

static void test_removal_kept(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A };
    world w;
    palette_report report;
    str_list list = {0};

    test_case("keeping them instead");
    world_build(&w, "pal_keep", "\"" PAL_A "\"", names, 1);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the tab's own colours");
    str_list_push(&list, str_dup(PAL_A));

    CHECK(palettes_remove(w.dig, &w.paths, &list, 1, &report, err, sizeof err) == 0,
          "the removal runs (%s)", err);
    CHECK_NUM(report.removed, 0, "nothing was deleted");
    check_outcome(&report, PAL_A, PAL_KEPT, NULL);
    CHECK(palette_there(w.game, PAL_A), "the palette is still there");

    palette_report_free(&report);
    str_list_free(&list);
    world_free(&w);
}

/*
 * What went in is remembered by name, which is what keeps an uninstall from
 * deleting a palette that was only skipped over.
 */
static void test_state_record(void)
{
    char err[TB_ERR_LEN];
    static const char *const names[] = { PAL_A, PAL_B };
    world w;
    palette_plan plan = {0};
    palette_report report;
    config *cfg;
    str_list made = {0}, read_back = {0};

    test_case("remembering which folders were made");
    world_build(&w, "pal_record", "\"" PAL_A "\", \"" PAL_B "\"", names, 2);
    if (!w.tab) { world_free(&w); return; }
    make_palette(w.game, PAL_A, "the user's own colours");

    CHECK(palettes_plan_build(w.dig, w.tab, &w.paths, PALETTE_SKIP, &plan,
                              err, sizeof err) == 0, "the plan is built (%s)", err);
    CHECK(palettes_plan_apply(&plan, &report, err, sizeof err) == 0,
          "and applied (%s)", err);
    palettes_plan_installed(&plan, &made);
    CHECK_NUM(made.count, 1, "only the folder we really made is remembered");
    if (made.count)
        CHECK_STR(made.items[0], PAL_B, "and it is the right one");

    cfg = config_load(err, sizeof err);
    if (cfg) {
        config_set_palettes(cfg, 0, TAB_CODE, &made);
        CHECK(config_save(cfg, err, sizeof err) == 0, "the record is written (%s)", err);
        config_free(cfg);
    } else {
        CHECK(0, "the state file loads (%s)", err);
    }

    cfg = config_load(err, sizeof err);
    if (cfg) {
        config_get_palettes(cfg, TAB_CODE, &read_back);
        CHECK_NUM(read_back.count, 1, "and read back");
        if (read_back.count)
            CHECK_STR(read_back.items[0], PAL_B, "unchanged");
        config_set_palettes(cfg, 0, TAB_CODE, NULL);
        config_free(cfg);
    }

    str_list_free(&made);
    str_list_free(&read_back);
    palette_report_free(&report);
    palettes_plan_free(&plan);
    world_free(&w);
}

void suite_palettes(void)
{
    test_suite("palettes");
    test_baked_names();
    test_collision_modes();
    test_fresh_install();
    test_no_palettes();
    test_collisions();
    test_case_insensitive();
    test_baked_collision();
    test_bundled_twice();
    test_limit();
    test_baked_folders_are_free();
    test_missing_source();
    test_undo();
    test_removal();
    test_removal_kept();
    test_state_record();
}
