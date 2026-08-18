/*
 * test_game.c - Installing and uninstalling against a stand-in N++ install.
 *
 * Everything here runs on a fake game built in a scratch directory, so the
 * suite exercises the real code paths without going anywhere near a real
 * installation. The digest fixture names its own server, which also keeps the
 * patching tests off the network.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "digest.h"
#include "install.h"
#include "json.h"
#include "patch.h"
#include "paths.h"
#include "platform.h"
#include "tabs.h"
#include "test.h"
#include "util.h"

/* What the fixture tab ships, and what the game should end up pointing at. */
#define TAB_CODE          "tst"
#define TAB_LEVEL_FILE    "SI.txt"
#define TAB_CHALLENGE     "Scodes.txt"
#define TAB_EXTRA_FILE    "README.txt"      /* not a file the game reads */
#define TAB_LEVEL_BODY    "custom level data"
#define TAB_CHALLENGE_BODY "AGNT AGNO"
#define EXPECTED_PATCH    "http://" TEST_DEAD_HOST ":9/" TAB_CODE

/* A whole world for one test: tool root, tab store, fake game. */
typedef struct {
    char *root;        /* the tool's root, holding config.json and tabs/ */
    char *install;     /* the fake installation directory                */
    char *levels;      /* the game's levels folder                       */
    char *library;     /* the fake main library                          */
    digest *dig;
    npp_paths paths;
} world;

static void world_build(world *w, const char *name)
{
    char err[TB_ERR_LEN];
    char *digest_path, *tab_levels, *path;

    memset(w, 0, sizeof(*w));
    w->root = test_dir(name);
    test_use_root(w->root);

    digest_path = digest_cache_path();
    test_write(digest_path, TEST_DIGEST_JSON);
    free(digest_path);

    w->install = test_fake_game(w->root);
    {
        char *assets = path_join(w->install, NPP_ASSETS_SUBDIR);
        w->levels = path_join(assets, DIGEST_DEFAULT_LEVELS_DIR);
        free(assets);
    }

    /* The tab as it would sit in the store after a fetch. */
    {
        char *tab_root = tab_dir_path(TAB_CODE);
        tab_levels = path_join(tab_root, DIGEST_DEFAULT_LEVELS_DIR);
        free(tab_root);
    }
    path = path_join(tab_levels, TAB_LEVEL_FILE);
    test_write(path, TAB_LEVEL_BODY);
    free(path);
    path = path_join(tab_levels, TAB_CHALLENGE);
    test_write(path, TAB_CHALLENGE_BODY);
    free(path);
    path = path_join(tab_levels, TAB_EXTRA_FILE);
    test_write(path, "notes that the game never reads");
    free(path);
    free(tab_levels);

    w->dig = digest_load(err, sizeof err);
    CHECK(w->dig != NULL, "the digest fixture loads (%s)", w->dig ? "" : err);
    CHECK(npp_find_game_dirs(&w->paths, err, sizeof err) == 0,
          "the fake game is found (%s)", err);
    w->library = lib_path(&w->paths);
}

static void world_free(world *w)
{
    digest_free(w->dig);
    npp_paths_free(&w->paths);
    free(w->root);
    free(w->install);
    free(w->levels);
    free(w->library);
}

static const npp_tab *world_tab(world *w, const char *code)
{
    const npp_tab *tab = digest_find(w->dig, code);
    CHECK(tab != NULL, "the digest knows '%s'", code);
    return tab;
}

/* Contents of a file in the game's levels folder. Caller frees. */
static char *game_file(world *w, const char *name)
{
    char *path = path_join(w->levels, name);
    char *text = test_read(path);

    free(path);
    return text;
}

/* Whether the library carries `uri`, and only recognised text. */
static void check_library(world *w, const char *expect_uri, const char *what)
{
    char err[TB_ERR_LEN];
    str_list known = {0};
    lib_image img;
    config *cfg = config_load(err, sizeof err);

    server_known_uris(cfg, w->dig, &known);
    if (lib_open(&w->paths, &known, &img, err, sizeof err) == 0) {
        CHECK_STR(img.uri, expect_uri, what);
        CHECK_NUM(img.occurrences, 1, "the URI appears exactly once");
        lib_close(&img);
    } else {
        CHECK(0, "%s: the library could not be read (%s)", what, err);
    }
    str_list_free(&known);
    config_free(cfg);
}

/* ---- The happy path ---------------------------------------------------- */

static void test_install_uninstall(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report removed;
    lib_health health;
    config *cfg;
    json_value *entry;
    char *text, *backup, *before;
    char install_date[64];

    test_case("install then uninstall");
    world_build(&w, "game_round");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* A clean game is healthy and points at the official server. */
    cfg = config_load(err, sizeof err);
    CHECK(lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err) == 0, "the check runs");
    CHECK(health.healthy, "a clean game passes: %s", health.detail);
    CHECK_NUM(health.state, LIB_ORIGINAL, "the library is original");
    config_free(cfg);

    /* Keep a copy of an original the tab is about to replace. */
    before = game_file(&w, TAB_LEVEL_FILE);

    CHECK(tab_install(w.dig, tab, &w.paths, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK_NUM(installed.installed_count, 2, "both supported files are installed");
    CHECK_NUM(installed.skipped.count, 1, "the unsupported file is skipped");
    if (installed.skipped.count)
        CHECK_STR(installed.skipped.items[0], TAB_EXTRA_FILE, "and it is named");

    /* The health check ran and failed — the fixture server is a dead port —
     * and that is a warning, not a reason to refuse the install. */
    CHECK_STR(installed.health.url, "http://" TEST_DEAD_HOST ":9/health",
              "the server was asked whether it is up");
    CHECK_NUM(installed.health.reachable, 0, "it is not");
    CHECK(installed.health.detail[0] != '\0', "and the reason is reported");

    /* The game now holds the tab's files... */
    text = game_file(&w, TAB_LEVEL_FILE);
    CHECK_STR(text, TAB_LEVEL_BODY, "the level file is the tab's");
    free(text);
    text = game_file(&w, TAB_CHALLENGE);
    CHECK_STR(text, TAB_CHALLENGE_BODY, "the challenge file is the tab's");
    free(text);

    /* ...with the originals set aside untouched. */
    backup = str_fmt("%s%s", TAB_LEVEL_FILE, INSTALL_BACKUP_SUFFIX);
    text = game_file(&w, backup);
    CHECK_STR(text, before, "the original is kept under its OG name");
    free(text);
    free(backup);

    /* Files the tab does not ship are left alone. */
    text = game_file(&w, "CT.txt");
    CHECK_STR(text, "original CT.txt", "an unrelated game file is untouched");
    free(text);
    text = game_file(&w, TAB_EXTRA_FILE);
    CHECK(text == NULL, "the skipped file was not copied into the game");
    free(text);

    check_library(&w, EXPECTED_PATCH, "the library points at the 3rd party server");

    cfg = config_load(err, sizeof err);
    entry = config_find_tab(cfg, TAB_CODE);
    CHECK(entry && json_get_bool(entry, CJK_INSTALLED, 0), "the state records the install");
    snprintf(install_date, sizeof install_date, "%s",
             json_get_string(entry, CJK_INSTALL_DATE, ""));
    CHECK(json_get_bool(json_get(cfg->root, CJK_STATE), CJK_LIBRARY, 0),
          "state.library is true");
    CHECK(lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err) == 0, "the check runs");
    CHECK(health.healthy, "an installed game passes: %s", health.detail);
    CHECK_NUM(health.state, LIB_PATCHED, "the library is patched");
    CHECK_STR(health.lib_code, TAB_CODE, "the library names the installed tab");
    config_free(cfg);

    /* Only one tab at a time. */
    {
        install_report second;
        const npp_tab *other = world_tab(&w, "oth");
        CHECK(tab_install(w.dig, other, &w.paths, &second, err, sizeof err) != 0,
              "installing a second tab is refused");
        install_report_free(&second);
    }

    /* Now put it all back. */
    CHECK(tab_uninstall(w.dig, tab, &w.paths, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    CHECK_NUM(removed.restored_count, 2, "both originals are restored");

    text = game_file(&w, TAB_LEVEL_FILE);
    CHECK_STR(text, before, "the original level file is back, byte for byte");
    free(text);
    backup = str_fmt("%s%s", TAB_LEVEL_FILE, INSTALL_BACKUP_SUFFIX);
    text = game_file(&w, backup);
    CHECK(text == NULL, "the OG backup is gone");
    free(text);
    free(backup);

    check_library(&w, LIB_OFFICIAL_URI, "the library points at the official server again");

    cfg = config_load(err, sizeof err);
    entry = config_find_tab(cfg, TAB_CODE);
    CHECK(!json_get_bool(entry, CJK_INSTALLED, 1), "the state records the uninstall");
    CHECK(json_get(entry, CJK_UNINSTALL_DATE)->type == JSON_STRING, "uninstall_date is stamped");
    CHECK_STR(json_get_string(entry, CJK_INSTALL_DATE, ""), install_date,
              "install_date is kept");
    CHECK(lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err) == 0, "the check runs");
    CHECK(health.healthy, "the game is clean again: %s", health.detail);
    config_free(cfg);

    free(before);
    install_report_free(&installed);
    uninstall_report_free(&removed);
    world_free(&w);
}

/* ---- Refusals, each of which must change nothing ----------------------- */

static void test_install_refusals(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report report;
    char *path, *before;

    test_case("an install that cannot go ahead");
    world_build(&w, "game_refuse");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* The game is missing a file the tab replaces. */
    path = path_join(w.levels, TAB_LEVEL_FILE);
    plat_remove_file(path);
    CHECK(tab_install(w.dig, tab, &w.paths, &report, err, sizeof err) != 0,
          "install is refused when a target file is missing");
    CHECK(strstr(err, TAB_LEVEL_FILE) != NULL, "the missing file is named");
    install_report_free(&report);
    check_library(&w, LIB_OFFICIAL_URI, "the library was not touched");
    test_write(path, "original " TAB_LEVEL_FILE);
    free(path);

    /* A backup from an install that was never undone. The pristine original
     * behind it must not be overwritten. */
    path = str_fmt("%s%c%s%s", w.levels, PATH_SEP, TAB_LEVEL_FILE, INSTALL_BACKUP_SUFFIX);
    test_write(path, "a precious original");
    CHECK(tab_install(w.dig, tab, &w.paths, &report, err, sizeof err) != 0,
          "install is refused when a backup already exists");
    install_report_free(&report);
    before = test_read(path);
    CHECK_STR(before, "a precious original", "the existing backup is intact");
    free(before);
    plat_remove_file(path);
    free(path);

    /* A library that is already patched. */
    {
        str_list known = {0};
        lib_image img;
        config *cfg = config_load(err, sizeof err);

        server_known_uris(cfg, w.dig, &known);
        CHECK(lib_open(&w.paths, &known, &img, err, sizeof err) == 0, "the library opens");
        CHECK(lib_write_uri(&img, "http://" TEST_DEAD_HOST ":9/oth", err, sizeof err) == 0,
              "the library can be patched by hand");
        lib_close(&img);
        str_list_free(&known);
        config_free(cfg);
    }
    CHECK(tab_install(w.dig, tab, &w.paths, &report, err, sizeof err) != 0,
          "install is refused when the library is already patched");
    CHECK(strstr(err, "not in a clean state") != NULL, "the check catches it first");
    install_report_free(&report);
    {
        char *text = game_file(&w, TAB_LEVEL_FILE);
        CHECK_STR(text, "original " TAB_LEVEL_FILE, "no level file was touched");
        free(text);
    }

    world_free(&w);
}

static void test_uninstall_refusals(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab, *other;
    install_report installed;
    uninstall_report report;
    char *path, *text;
    config *cfg;

    test_case("an uninstall that cannot go ahead");
    world_build(&w, "game_refuse2");
    tab = world_tab(&w, TAB_CODE);
    other = world_tab(&w, "oth");
    if (!tab || !other || !w.dig) { world_free(&w); return; }

    /* Nothing is installed at all. */
    CHECK(tab_uninstall(w.dig, tab, &w.paths, &report, err, sizeof err) != 0,
          "uninstall is refused on a clean game");
    uninstall_report_free(&report);

    CHECK(tab_install(w.dig, tab, &w.paths, &installed, err, sizeof err) == 0,
          "install for the next checks (%s)", err);
    install_report_free(&installed);

    /* A backup has gone missing: restoring would leave the game short. */
    path = str_fmt("%s%c%s%s", w.levels, PATH_SEP, TAB_CHALLENGE, INSTALL_BACKUP_SUFFIX);
    text = test_read(path);
    plat_remove_file(path);
    CHECK(tab_uninstall(w.dig, tab, &w.paths, &report, err, sizeof err) != 0,
          "uninstall is refused when a backup is missing");
    CHECK(strstr(err, TAB_CHALLENGE) != NULL, "the file without a backup is named");
    uninstall_report_free(&report);
    /* Nothing may have been half-restored. */
    {
        char *level = game_file(&w, TAB_LEVEL_FILE);
        CHECK_STR(level, TAB_LEVEL_BODY, "the other files are still the tab's");
        free(level);
    }
    check_library(&w, EXPECTED_PATCH, "the library is still patched");
    test_write(path, text);
    free(text);
    free(path);

    /* Asking to uninstall a tab the library does not name. */
    cfg = config_load(err, sizeof err);
    config_set_installed(cfg, 1, "oth");     /* pretend the state drifted */
    config_set_uninstalled(cfg, 0, TAB_CODE);
    config_save(cfg, err, sizeof err);
    config_free(cfg);

    CHECK(tab_uninstall(w.dig, other, &w.paths, &report, err, sizeof err) != 0,
          "uninstalling the wrong tab is refused");
    CHECK(strstr(err, TAB_CODE) != NULL, "the tab the library names is reported");
    uninstall_report_free(&report);
    check_library(&w, EXPECTED_PATCH, "the library is untouched");

    world_free(&w);
}

/* The check has to notice both ways a game can drift out of step. */
static void test_health_mismatches(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    lib_health health;
    config *cfg;

    test_case("the library check catches drift");
    world_build(&w, "game_health");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* Recorded as installed, but the library says otherwise. */
    cfg = config_load(err, sizeof err);
    config_set_installed(cfg, 0, TAB_CODE);
    lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err);
    CHECK(!health.healthy, "a phantom install is caught");
    CHECK(strstr(health.detail, "official server") != NULL, "...and explained");
    CHECK(!json_get_bool(json_get(cfg->root, CJK_STATE), CJK_LIBRARY, 1),
          "state.library is recorded as false");
    config_set_uninstalled(cfg, 0, TAB_CODE);
    config_save(cfg, err, sizeof err);
    config_free(cfg);

    /* Patched, but nothing is recorded as installed. */
    CHECK(tab_install(w.dig, tab, &w.paths, &installed, err, sizeof err) == 0,
          "install (%s)", err);
    install_report_free(&installed);
    cfg = config_load(err, sizeof err);
    config_set_uninstalled(cfg, 0, TAB_CODE);   /* forget it on purpose */
    lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err);
    CHECK(!health.healthy, "an unrecorded install is caught");
    CHECK(strstr(health.detail, "no tab is recorded") != NULL, "...and explained");
    config_free(cfg);

    /* A library pointing somewhere we do not know at all. */
    {
        str_list known = {0};
        lib_image img;
        cfg = config_load(err, sizeof err);
        server_known_uris(cfg, w.dig, &known);
        if (lib_open(&w.paths, &known, &img, err, sizeof err) == 0) {
            lib_write_uri(&img, "http://stranger.invalid/xx", err, sizeof err);
            lib_close(&img);
        }
        str_list_free(&known);
        lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err);
        CHECK(!health.healthy, "an unrecognised URI is caught");
        CHECK_NUM(health.state, LIB_UNKNOWN, "...and reported as unknown");
        config_free(cfg);
    }

    world_free(&w);
}

/* ---- The local store --------------------------------------------------- */

static void test_remove(void)
{
    char err[TB_ERR_LEN];
    world w;
    tab_remove_report report;
    config *cfg;
    json_value *entry;
    char remove_date[64];

    test_case("removing a downloaded tab");
    world_build(&w, "game_remove");

    cfg = config_load(err, sizeof err);
    config_set_downloaded(cfg, 0, TAB_CODE);
    config_save(cfg, err, sizeof err);
    config_free(cfg);

    CHECK(tab_is_downloaded(TAB_CODE), "the tab is in the store");
    CHECK(tab_remove(TAB_CODE, 0, &report, err, sizeof err) == 0, "remove succeeds (%s)", err);
    CHECK(report.had_files, "it had files to delete");
    CHECK(report.recorded, "the removal was recorded");
    CHECK(!tab_is_downloaded(TAB_CODE), "the tab is gone from the store");
    tab_remove_report_free(&report);

    cfg = config_load(err, sizeof err);
    entry = config_find_tab(cfg, TAB_CODE);
    CHECK(entry != NULL, "the entry is kept, so history survives");
    CHECK(!json_get_bool(entry, CJK_DOWNLOADED, 1), "downloaded is cleared");
    snprintf(remove_date, sizeof remove_date, "%s", json_get_string(entry, CJK_REMOVE_DATE, ""));
    CHECK(remove_date[0] != '\0', "remove_date is stamped");
    config_free(cfg);

    /* Removing it again changes nothing and must not restamp the date. */
    CHECK(tab_remove(TAB_CODE, 0, &report, err, sizeof err) == 0, "a second remove still succeeds");
    CHECK(!report.had_files, "there was nothing left to delete");
    CHECK(!report.recorded, "and nothing new to record");
    tab_remove_report_free(&report);
    cfg = config_load(err, sizeof err);
    CHECK_STR(json_get_string(config_find_tab(cfg, TAB_CODE), CJK_REMOVE_DATE, ""),
              remove_date, "remove_date is not restamped");
    config_free(cfg);

    world_free(&w);
}

/* A code becomes a directory name that gets deleted recursively. */
static void test_codes(void)
{
    static const char *bad[] = { "..", "../..", "a/b", "a\\b", ".", "me t", "", NULL };
    static const char *good[] = { "met", "ctp", "a1", "TST", NULL };
    char err[TB_ERR_LEN];
    tab_remove_report report;
    int i;

    test_case("tab codes are vetted before use as paths");
    for (i = 0; bad[i]; i++) {
        CHECK(!tab_code_is_valid(bad[i]), "'%s' is rejected", bad[i]);
        CHECK(tab_remove(bad[i], -1, &report, err, sizeof err) != 0,
              "remove refuses '%s'", bad[i]);
        tab_remove_report_free(&report);
    }
    for (i = 0; good[i]; i++)
        CHECK(tab_code_is_valid(good[i]), "'%s' is accepted", good[i]);
    CHECK(!tab_code_is_valid("thiscodeiswaytoolongtobereal"), "an over-long code is rejected");
    CHECK(!tab_code_is_valid(NULL), "a null code is rejected");
}

void suite_game(void)
{
    test_suite("game");
    test_install_uninstall();
    test_install_refusals();
    test_uninstall_refusals();
    test_health_mismatches();
    test_remove();
    test_codes();
}
