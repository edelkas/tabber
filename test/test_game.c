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

#include "cloud.h"
#include "config.h"
#include "digest.h"
#include "gzip.h"
#include "install.h"
#include "json.h"
#include "keys.h"
#include "palettes.h"
#include "patch.h"
#include "paths.h"
#include "platform.h"
#include "save.h"
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
#define TAB_PALETTE       "test palette"    /* the palette the fixture bundles */
#define TAB_SWATCH        "background.tga"
#define TAB_SWATCH_BODY   "colours in a file"
#define EXPECTED_PATCH    "http://" TEST_DEAD_HOST ":9/" TAB_CODE

/* The lines of the game's string table once this tab is installed, and the
 * ones the previous installer used to leave behind in English. */
#define LOC_ID_SHORT   "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT"
#define LOC_DONE_LONG  "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG|" \
                       "Speedrun Boards|Speedrun Boards\n"
#define LOC_DONE_SHORT LOC_ID_SHORT "|Speedrun|Speedrun\n"
#define LOC_DONE_PRESS "PLAYER_PRESS_ANY|Test Tab|Test Tab\n"
#define LOC_OLD_TABLE  "LOC_ID|english|spanish\n" \
                       "EPISODE|Episode|Episodio\n" \
                       "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG|" \
                       "Speedrun Boards|Records de amigos\n" \
                       LOC_ID_SHORT "|Speedrun|Amigos\n" \
                       "PLAYER_PRESS_ANY|Metanet|Pulsa una tecla\n" \
                       "LEVEL|Level|Nivel\n"

/*
 * The bindings file the fake personal folder starts with. The fixture tab's
 * digest entry asks for players 1 and 2 to share one set of controls, so
 * installing it should leave the file reading KEYS_BOUND and put the three
 * originals — one of them unbound to begin with — on record.
 */
#define KEYS_P1_LEFT  "KEYBIND(\"Left\")"
#define KEYS_P1_RIGHT "KEYBIND(\"Right\")"
#define KEYS_P1_JUMP  "KEYBIND(\"Z\")"
#define KEYS_P2_LEFT  "KEYBIND(\"A\")"
#define KEYS_P2_RIGHT "KEYBIND(\"D\")"
#define KEYS_P2_JUMP  KEYS_UNBOUND
#define KEYS_P1_BLOCK "//Player 1\n" \
                      "input_p1_left_key = " KEYS_P1_LEFT ";\n" \
                      "input_p1_right_key = " KEYS_P1_RIGHT ";\n" \
                      "input_p1_jump_key = " KEYS_P1_JUMP ";\n" \
                      "input_p1_back_key = KEYBIND(\"X\");\n" \
                      "\n"
#define KEYS_P2_BLOCK(left, right, jump) \
                      "//Player 2\n" \
                      "input_p2_left_key = " left ";\n" \
                      "input_p2_right_key = " right ";\n" \
                      "input_p2_jump_key = " jump ";\n" \
                      "input_p2_back_key = KEYBIND(\"R\");\n"
#define KEYS_TABLE    "//Created automatically. Modify at your own risk\n" \
                      KEYS_P1_BLOCK KEYS_P2_BLOCK(KEYS_P2_LEFT, KEYS_P2_RIGHT, KEYS_P2_JUMP)
#define KEYS_BOUND    "//Created automatically. Modify at your own risk\n" \
                      KEYS_P1_BLOCK KEYS_P2_BLOCK(KEYS_P1_LEFT, KEYS_P1_RIGHT, KEYS_P1_JUMP)

/* The savefile the fake game starts with, and the one tabber "ships". */
#define GAME_SAVE         "the player's own save"
#define FRESH_SAVE        "the fresh save tabber ships"

/* Steam's own copy of it, and the account that holds it. */
#define CLOUD_SAVE        "what Steam has been keeping"
#define CLOUD_ACCOUNT     "76561198000000042"

/* A whole world for one test: tool root, tab store, fake game. */
typedef struct {
    char *root;        /* the tool's root, holding config.json and tabs/ */
    char *install;     /* the fake installation directory                */
    char *levels;      /* the game's levels folder                       */
    char *library;     /* the fake main library                          */
    char *palettes;    /* the game's palettes folder                     */
    char *personal;    /* the fake personal folder, holding the savefile */
    char *fresh;       /* the fresh savefile tabber would ship           */
    char *cloud;       /* the fake Steam Cloud folder of one account     */
    char *pristine_library;   /* a copy of the library as the game shipped it */
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
        w->palettes = path_join(assets, DIGEST_DEFAULT_PALETTES_DIR);
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

    /* ...and the palette it bundles, a folder of swatches of its own. */
    {
        char *tab_root = tab_dir_path(TAB_CODE);
        char *pal = path_join(tab_root, DIGEST_DEFAULT_PALETTES_DIR);
        char *folder = path_join(pal, TAB_PALETTE);

        path = path_join(folder, TAB_SWATCH);
        test_write(path, TAB_SWATCH_BODY);
        free(path);
        free(folder);
        free(pal);
        free(tab_root);
    }

    /* The personal folder, with a savefile in it, and the shipped fresh one.
     * The game's bindings file lives there too, beside the save. */
    w->personal = test_fake_personal(w->root, SAVE_NAME, GAME_SAVE, strlen(GAME_SAVE));
    path = path_join(w->personal, KEYS_FILE_NAME);
    test_write(path, KEYS_TABLE);
    free(path);
    w->fresh = path_join(w->root, "fresh_nprofile.zip");
    test_write_zip(w->fresh, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE));
    test_use_fresh_save(w->fresh);

    /* A Steam folder with one account that has a cloud save of its own. */
    {
        char *steam = path_join(w->root, "Steam");
        char *userdata = path_join(steam, CLOUD_USERDATA_DIR);
        char *account = path_join(userdata, CLOUD_ACCOUNT);
        char *app = path_join(account, NPP_STEAM_APPID);
        unsigned char *gz;
        size_t gz_len = 0;

        w->cloud = path_join(app, CLOUD_REMOTE_DIR);
        plat_mkdir_p(w->cloud);
        gz = test_gzip(CLOUD_SAVE, strlen(CLOUD_SAVE), &gz_len);
        path = path_join(w->cloud, SAVE_GZ_NAME);
        test_write_bytes(path, gz, gz_len);
        free(path);
        free(gz);

        test_setenv(TABBER_ENV_STEAM_DIR, steam);
        free(steam);
        free(userdata);
        free(account);
        free(app);
    }

    /* A copy of the untouched library, so a round trip can be judged on the
     * whole file rather than on the strings we happen to look at. */
    w->pristine_library = path_join(w->root, "npp.dll.pristine");

    w->dig = digest_load(err, sizeof err);
    CHECK(w->dig != NULL, "the digest fixture loads (%s)", w->dig ? "" : err);
    CHECK(npp_find_game_dirs(&w->paths, err, sizeof err) == 0,
          "the fake game is found (%s)", err);
    CHECK(npp_find_personal_dir(&w->paths, err, sizeof err) == 0,
          "so is its personal folder (%s)", err);
    w->library = lib_path(&w->paths);
    {
        size_t len = 0;
        unsigned char *image = test_read_bytes(w->library, &len);

        if (image)
            test_write_bytes(w->pristine_library, image, len);
        free(image);
    }
}

static void world_free(world *w)
{
    test_use_fresh_save(NULL);
    test_setenv(TABBER_ENV_STEAM_DIR, NULL);
    digest_free(w->dig);
    npp_paths_free(&w->paths);
    free(w->root);
    free(w->install);
    free(w->levels);
    free(w->palettes);
    free(w->library);
    free(w->personal);
    free(w->fresh);
    free(w->cloud);
    free(w->pristine_library);
}

/* The savefile Steam is holding, unwrapped. Caller frees. */
static unsigned char *cloud_save(world *w, size_t *len_out)
{
    char err[TB_ERR_LEN];
    char *path = path_join(w->cloud, SAVE_GZ_NAME);
    size_t raw_len = 0;
    unsigned char *raw = test_read_bytes(path, &raw_len);
    unsigned char *plain = NULL;

    free(path);
    if (raw && gz_is_gzip(raw, raw_len))
        plain = gz_extract(raw, raw_len, len_out, err, sizeof err);
    free(raw);
    return plain;
}

/* Contents of a file in the personal folder. Caller frees. */
static char *personal_file(world *w, const char *name)
{
    char *path = path_join(w->personal, name);
    char *text = test_read(path);

    free(path);
    return text;
}

/* Whether a file is in the personal folder. */
static int personal_has(world *w, const char *name)
{
    char *path = path_join(w->personal, name);
    int there = plat_is_file(path);

    free(path);
    return there;
}

static const npp_tab *world_tab(world *w, const char *code)
{
    const npp_tab *tab = digest_find(w->dig, code);
    CHECK(tab != NULL, "the digest knows '%s'", code);
    return tab;
}

/* Contents of a palette's swatch in the game's folder. Caller frees. */
static char *palette_file(world *w, const char *name)
{
    char *folder = path_join(w->palettes, name);
    char *path = path_join(folder, TAB_SWATCH);
    char *text = test_read(path);

    free(path);
    free(folder);
    return text;
}

/* The game's string table as it now reads. Caller frees. */
static char *loc_table(world *w)
{
    char *assets = path_join(w->install, NPP_ASSETS_SUBDIR);
    char *path = path_join(assets, LOC_FILE_NAME);
    char *text = test_read(path);

    free(path);
    free(assets);
    return text;
}

/* Overwrites it, which is how a table the old installer edited is set up. */
static void set_loc_table(world *w, const char *text)
{
    char *assets = path_join(w->install, NPP_ASSETS_SUBDIR);
    char *path = path_join(assets, LOC_FILE_NAME);

    test_write(path, text);
    free(path);
    free(assets);
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

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
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

    /* The palette it bundles was copied into the game's own folder, which the
     * fake install ships without, exactly as the real Windows build does. */
    CHECK_NUM(installed.palettes.count, 1, "the bundled palette is reported");
    CHECK_NUM(installed.palettes.installed, 1, "and it went in");
    text = palette_file(&w, TAB_PALETTE);
    CHECK_STR(text, TAB_SWATCH_BODY, "its swatches came across");
    free(text);

    /* The game's own texts were replaced, in both languages the table has. */
    CHECK_NUM(installed.strings.changed, 6, "three strings in two languages");
    text = loc_table(&w);
    CHECK(strstr(text, LOC_DONE_LONG) != NULL, "the friend boards are renamed");
    CHECK(strstr(text, LOC_DONE_SHORT) != NULL, "the short label with them");
    CHECK(strstr(text, LOC_DONE_PRESS) != NULL, "and the title screen names the tab");
    CHECK(strstr(text, "LEVEL|Level|Nivel") != NULL,
          "a string the tab does not replace is left alone");
    free(text);

    /* The savefile was swapped as part of the same install. */
    text = personal_file(&w, SAVE_NAME);
    CHECK_STR(text, FRESH_SAVE, "the tab's savefile is in place");
    free(text);
    CHECK(personal_has(&w, SAVE_BACKUP_ORIGINAL), "the player's own save was archived");
    CHECK(installed.save.backed_up, "and the report says so");
    CHECK(installed.save.used_fresh, "this tab had no save of its own yet");

    /* Steam's copy takes precedence over the local one, so it has to have been
     * brought in step with it — gzipped, which is all the cloud takes. */
    CHECK_NUM(installed.cloud.count, 1, "the one Steam account was found");
    CHECK(installed.cloud.users && installed.cloud.users[0].replaced,
          "and its cloud save was replaced");
    {
        size_t cloud_len = 0;
        unsigned char *cloud = cloud_save(&w, &cloud_len);

        CHECK(cloud && cloud_len == strlen(FRESH_SAVE) &&
              memcmp(cloud, FRESH_SAVE, cloud_len) == 0,
              "with the save that went into the game");
        free(cloud);
    }

    cfg = config_load(err, sizeof err);
    entry = config_find_tab(cfg, TAB_CODE);
    CHECK(entry && json_get_bool(entry, CJK_INSTALLED, 0), "the state records the install");
    snprintf(install_date, sizeof install_date, "%s",
             json_get_string(entry, CJK_INSTALL_DATE, ""));
    CHECK(json_get_bool(json_get(cfg->root, CJK_STATE), CJK_LIBRARY, 0),
          "state.library is true");
    CHECK_STR(json_get_string(json_get(config_get_strings(cfg), LOC_ID_SHORT),
                              "english", ""), "Friends",
              "and the texts that were overwritten are recorded with their originals");
    CHECK(lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err) == 0, "the check runs");
    CHECK(health.healthy, "an installed game passes: %s", health.detail);
    CHECK_NUM(health.state, LIB_PATCHED, "the library is patched");
    CHECK_STR(health.lib_code, TAB_CODE, "the library names the installed tab");
    config_free(cfg);

    /* Only one tab at a time. */
    {
        install_report second;
        const npp_tab *other = world_tab(&w, "oth");
        CHECK(tab_install(w.dig, other, &w.paths, NULL, &second, err, sizeof err) != 0,
              "installing a second tab is refused");
        install_report_free(&second);
    }

    /* Now put it all back. */
    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &removed, err, sizeof err) == 0,
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

    text = loc_table(&w);
    CHECK_STR(text, TEST_LOC_TABLE, "the game's texts are back, byte for byte");
    free(text);

    CHECK_NUM(removed.palettes.removed, 1, "the palette it brought was taken out again");
    text = palette_file(&w, TAB_PALETTE);
    CHECK(text == NULL, "and its folder is gone from the game");
    free(text);

    text = personal_file(&w, SAVE_NAME);
    CHECK_STR(text, GAME_SAVE, "the player's savefile is back, byte for byte");
    free(text);
    CHECK(personal_has(&w, "nprofile_" TAB_CODE ".zip"),
          "and the tab's save was kept for next time");
    {
        size_t cloud_len = 0;
        unsigned char *cloud = cloud_save(&w, &cloud_len);

        CHECK(cloud && cloud_len == strlen(GAME_SAVE) &&
              memcmp(cloud, GAME_SAVE, cloud_len) == 0,
              "and Steam's copy came back with it");
        free(cloud);
    }

    cfg = config_load(err, sizeof err);
    entry = config_find_tab(cfg, TAB_CODE);
    CHECK(!json_get_bool(entry, CJK_INSTALLED, 1), "the state records the uninstall");
    CHECK(json_get(entry, CJK_UNINSTALL_DATE)->type == JSON_STRING, "uninstall_date is stamped");
    CHECK_STR(json_get_string(entry, CJK_INSTALL_DATE, ""), install_date,
              "install_date is kept");
    CHECK_NUM(json_count(config_get_strings(cfg)), 0,
              "the record of replaced texts is emptied, since none is live now");
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
    char *path;

    test_case("an install that cannot go ahead");
    world_build(&w, "game_refuse");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* The game is missing a file the tab replaces. */
    path = path_join(w.levels, TAB_LEVEL_FILE);
    plat_remove_file(path);
    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) != 0,
          "install is refused when a target file is missing");
    CHECK(strstr(err, TAB_LEVEL_FILE) != NULL, "the missing file is named");
    install_report_free(&report);
    check_library(&w, LIB_OFFICIAL_URI, "the library was not touched");
    test_write(path, "original " TAB_LEVEL_FILE);
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
    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) != 0,
          "install is refused when the library is already patched");
    CHECK(strstr(err, "already points at") != NULL,
          "because the library already names a tab (%s)", err);
    install_report_free(&report);
    {
        char *text = game_file(&w, TAB_LEVEL_FILE);
        CHECK_STR(text, "original " TAB_LEVEL_FILE, "no level file was touched");
        free(text);
    }

    /* Nothing that was refused above may have gone near the savefile. */
    {
        char *text = personal_file(&w, SAVE_NAME);
        CHECK_STR(text, GAME_SAVE, "the savefile came through every refusal intact");
        free(text);
        CHECK(!personal_has(&w, SAVE_BACKUP_ORIGINAL), "and nothing was archived");
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
    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) != 0,
          "uninstall is refused on a clean game");
    uninstall_report_free(&report);

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install for the next checks (%s)", err);
    install_report_free(&installed);

    /*
     * A backup has gone missing and the vanilla mappack cannot be had either:
     * its download in the fixture points at the dead port. With nothing left
     * to restore from, the uninstall is refused — and only then.
     */
    path = str_fmt("%s%c%s%s", w.levels, PATH_SEP, TAB_CHALLENGE, INSTALL_BACKUP_SUFFIX);
    text = test_read(path);
    plat_remove_file(path);
    CHECK(!tab_is_downloaded(INSTALL_ORIGINALS_CODE), "the originals are not in the store");
    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) != 0,
          "uninstall is refused when a backup is missing and cannot be replaced");
    CHECK(strstr(err, INSTALL_BACKUP_SUFFIX) != NULL,
          "and says what it was looking for (%s)", err);
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

    CHECK(tab_uninstall(w.dig, other, &w.paths, NULL, &report, err, sizeof err) != 0,
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

    /*
     * Patched, with nothing recorded as installed. That is not drift but the
     * signature of an installer that came before tabber, so it passes — and
     * says which tab it found, since uninstalling has to work from here.
     */
    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install (%s)", err);
    install_report_free(&installed);
    cfg = config_load(err, sizeof err);
    config_set_uninstalled(cfg, 0, TAB_CODE);   /* forget it on purpose */
    lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err);
    CHECK(health.healthy, "an install nothing recorded is accepted: %s", health.detail);
    CHECK(health.unrecorded, "...and marked as one we did not make");
    CHECK_STR(health.lib_code, TAB_CODE, "the library says which tab it is");
    CHECK_STR(health.state_code, "", "though nothing is on record");
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

/*
 * A palette of the user's own, with the same name as one the tab bundles. It
 * is skipped on the way in, and so must survive the way out: the uninstall
 * goes by what the install recorded, not by what the tab happens to ship.
 */
static void test_palette_not_ours(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report removed;
    char *folder, *path, *text;

    test_case("a palette we did not install is not removed");
    world_build(&w, "game_palette");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    folder = path_join(w.palettes, TAB_PALETTE);
    path = path_join(folder, TAB_SWATCH);
    test_write(path, "the player's own colours");
    free(path);
    free(folder);

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK_NUM(installed.palettes.installed, 0, "the bundled palette is skipped");
    text = palette_file(&w, TAB_PALETTE);
    CHECK_STR(text, "the player's own colours", "theirs is left as it was");
    free(text);

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    CHECK_NUM(removed.palettes.removed, 0, "and nothing is deleted on the way out");
    text = palette_file(&w, TAB_PALETTE);
    CHECK_STR(text, "the player's own colours", "their palette is still theirs");
    free(text);

    install_report_free(&installed);
    uninstall_report_free(&removed);
    world_free(&w);
}

/* A code becomes a directory name that gets deleted recursively. */
/*
 * --languages picks the columns, and the choice reaches all the way through an
 * install: with none, the table is not touched at all.
 */
static void test_texts_by_language(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_options opts;
    install_report installed;
    uninstall_report removed;
    loc_langs langs;
    config *cfg;
    char *text;

    test_case("--languages decides which columns an install writes");
    world_build(&w, "game_texts");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    install_options_init(&opts);
    loc_langs_parse("SPANISH, klingon", &langs);
    opts.languages = &langs;

    CHECK(tab_install(w.dig, tab, &w.paths, &opts, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK_NUM(installed.strings.changed, 3, "three strings, in the one language asked for");
    CHECK_NUM(installed.strings.unknown.count, 1, "the language it does not have is named");

    text = loc_table(&w);
    CHECK(strstr(text, LOC_ID_SHORT "|Friends|Speedrun\n") != NULL,
          "Spanish is replaced and English is left as the game shipped it");
    free(text);

    cfg = config_load(err, sizeof err);
    CHECK(json_get(json_get(config_get_strings(cfg), LOC_ID_SHORT), "english") == NULL,
          "only the language that changed is recorded");
    config_free(cfg);

    CHECK(tab_uninstall(w.dig, tab, &w.paths, &opts, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    text = loc_table(&w);
    CHECK_STR(text, TEST_LOC_TABLE, "and the table is the game's own again");
    free(text);

    install_report_free(&installed);
    uninstall_report_free(&removed);
    loc_langs_free(&langs);
    world_free(&w);

    test_case("...and 'none' leaves the game's texts entirely alone");
    world_build(&w, "game_texts_none");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    install_options_init(&opts);
    loc_langs_parse("none", &langs);
    opts.languages = &langs;

    CHECK(tab_install(w.dig, tab, &w.paths, &opts, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK_NUM(installed.strings.count, 0, "no string is even considered");
    text = loc_table(&w);
    CHECK_STR(text, TEST_LOC_TABLE, "the table is untouched, byte for byte");
    free(text);

    cfg = config_load(err, sizeof err);
    CHECK_NUM(json_count(config_get_strings(cfg)), 0, "and there is nothing to record");
    config_free(cfg);

    install_report_free(&installed);
    loc_langs_free(&langs);
    world_free(&w);
}

/*
 * The drop-in case: a game whose English texts were changed by the installer
 * that came before tabber, which recorded nothing anywhere. Uninstalling has to
 * put those back too, from the originals tabber carries.
 */
static void test_texts_from_the_old_installer(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_options opts;
    install_report installed;
    uninstall_report removed;
    loc_langs langs;
    char *text;

    test_case("texts the previous installer left behind are restored as well");
    world_build(&w, "game_texts_legacy");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* The game as that installer would have left it: English replaced, in a
     * table nothing in the state file knows anything about. */
    set_loc_table(&w, LOC_OLD_TABLE);

    install_options_init(&opts);
    loc_langs_parse("none", &langs);      /* so tabber records nothing itself */
    opts.languages = &langs;

    CHECK(tab_install(w.dig, tab, &w.paths, &opts, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    text = loc_table(&w);
    CHECK_STR(text, LOC_OLD_TABLE, "the install leaves the old texts as they are");
    free(text);

    CHECK(tab_uninstall(w.dig, tab, &w.paths, &opts, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    CHECK_NUM(removed.strings.changed, 3, "the three English strings go back");
    text = loc_table(&w);
    CHECK_STR(text, TEST_LOC_TABLE,
              "and the table is the game's own again, though nothing recorded it");
    free(text);

    install_report_free(&installed);
    uninstall_report_free(&removed);
    loc_langs_free(&langs);
    world_free(&w);
}

/*
 * The digest is what asks for the controls to be shared: the fixture tab
 * carries "bind": [1, 2], so installing it should do exactly what `bind 1,2`
 * does, and uninstalling should undo it from the same record.
 */
static void test_controls_bound_by_the_tab(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report removed;
    const json_value *record;
    config *cfg;
    char *text;

    test_case("a tab whose digest asks for it gets the controls bound");
    world_build(&w, "game_bind");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK_STR(installed.warning, "", "with nothing to warn about");
    CHECK_NUM(installed.bindings.changed, 3, "player 2's three controls are bound");
    CHECK_NUM(installed.bindings.source, 1, "to player 1's");

    text = personal_file(&w, KEYS_FILE_NAME);
    CHECK_STR(text, KEYS_BOUND, "and the file says so, disturbing nothing else");
    free(text);

    /* The originals, so the change can be undone: the unbound one included. */
    cfg = config_load(err, sizeof err);
    record = config_get_keybindings(cfg);
    CHECK_NUM(json_count(record), 3, "three originals are on record");
    CHECK_STR(json_get_string(record, "input_p2_left_key", ""), KEYS_P2_LEFT,
              "the key player 2 used to answer to");
    CHECK_STR(json_get_string(record, "input_p2_jump_key", ""), KEYS_UNBOUND,
              "and the control that had no key at all");
    CHECK(json_get(record, "input_p1_left_key") == NULL,
          "player 1 is not recorded: nothing of theirs was changed");
    config_free(cfg);

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    CHECK_NUM(removed.bindings.changed, 3, "the three go back");
    text = personal_file(&w, KEYS_FILE_NAME);
    CHECK_STR(text, KEYS_TABLE, "and the file is the player's own again");
    free(text);

    cfg = config_load(err, sizeof err);
    CHECK_NUM(json_count(config_get_keybindings(cfg)), 0,
              "with nothing left on record");
    config_free(cfg);

    install_report_free(&installed);
    uninstall_report_free(&removed);
    world_free(&w);
}

/*
 * A player who has never run the game has no bindings file. That is no reason
 * to refuse the tab: it is installed, the controls are not touched, and the
 * report says so.
 */
static void test_controls_without_a_bindings_file(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report removed;
    config *cfg;
    char *path;

    test_case("a missing bindings file does not stop an install");
    world_build(&w, "game_bind_missing");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    path = path_join(w.personal, KEYS_FILE_NAME);
    plat_remove_file(path);
    free(path);

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK(installed.warning[0] != '\0', "and says the controls were left alone");
    CHECK(installed.bindings.path == NULL, "no bindings were worked out");
    CHECK(!personal_has(&w, KEYS_FILE_NAME), "no bindings file was invented");

    cfg = config_load(err, sizeof err);
    CHECK_NUM(json_count(config_get_keybindings(cfg)), 0, "and nothing is on record");
    config_free(cfg);

    /* Nor does it stop the uninstall: there is simply nothing to put back. */
    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    CHECK(removed.bindings.path == NULL, "with no bindings to restore");

    install_report_free(&installed);
    uninstall_report_free(&removed);
    world_free(&w);
}

/*
 * The installers that came before tabber kept no backups: they shipped the
 * originals and wrote them back. So an "OG" file may be missing when a tab is
 * installed, and may be there when none is, and neither may be read as proof
 * of anything. The originals then come from the vanilla mappack.
 */

/* Puts the vanilla mappack in the store, holding `body` for each file. */
static void world_add_originals(world *w, const char *body)
{
    char *root = tab_dir_path(INSTALL_ORIGINALS_CODE);
    char *levels = path_join(root, DIGEST_DEFAULT_LEVELS_DIR);
    char *path;

    (void)w;
    path = path_join(levels, TAB_LEVEL_FILE);
    test_write(path, body);
    free(path);
    path = path_join(levels, TAB_CHALLENGE);
    test_write(path, body);
    free(path);
    free(levels);
    free(root);
}

/* The 'OG' file of one of the game's files. Caller frees. */
static char *backup_path(world *w, const char *name)
{
    return str_fmt("%s%c%s%s", w->levels, PATH_SEP, name, INSTALL_BACKUP_SUFFIX);
}

static void test_install_over_a_stale_backup(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report report;
    char *path, *text;

    test_case("a backup left behind by an older installer is not in the way");
    world_build(&w, "game_stale_backup");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /*
     * What an install by tabber followed by an uninstall by an older one
     * leaves: the game's own file back in place, and our backup of it still
     * sitting there. The library says no tab is installed, which is what
     * makes the backup a leftover rather than somebody's only original.
     */
    path = backup_path(&w, TAB_LEVEL_FILE);
    test_write(path, "a leftover from last time");

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) == 0,
          "the install goes ahead (%s)", err);
    CHECK_NUM(report.stale_backups.count, 1, "and says one backup was already there");
    if (report.stale_backups.count)
        CHECK_STR(report.stale_backups.items[0], TAB_LEVEL_FILE, "naming which");

    text = test_read(path);
    CHECK_STR(text, "original " TAB_LEVEL_FILE,
              "the backup now holds the file that was really there");
    free(text);
    text = game_file(&w, TAB_LEVEL_FILE);
    CHECK_STR(text, TAB_LEVEL_BODY, "and the tab's copy is installed over it");
    free(text);

    install_report_free(&report);
    free(path);
    world_free(&w);
}

static void test_uninstall_without_backups(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report report;
    char *path, *text;

    test_case("an uninstall falls back to the vanilla mappack for what has no backup");
    world_build(&w, "game_no_backup");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install first (%s)", err);
    install_report_free(&installed);

    /* One backup goes missing, as if that file had been installed by an older
     * installer, which kept none. The vanilla mappack is in the store. */
    path = backup_path(&w, TAB_CHALLENGE);
    plat_remove_file(path);
    world_add_originals(&w, "the vanilla file");

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) == 0,
          "the uninstall goes ahead (%s)", err);
    CHECK_NUM(report.restored_count, 2, "both files are restored");
    CHECK_NUM(report.from_backups, 1, "one from its own backup");
    CHECK_NUM(report.from_originals, 1, "and one from the vanilla mappack");
    CHECK_STR(report.originals_code, INSTALL_ORIGINALS_CODE, "which is named");
    CHECK(!report.fetched_originals, "already in the store, so nothing was downloaded");

    text = game_file(&w, TAB_LEVEL_FILE);
    CHECK_STR(text, "original " TAB_LEVEL_FILE, "the backed-up file is its own original");
    free(text);
    text = game_file(&w, TAB_CHALLENGE);
    CHECK_STR(text, "the vanilla file", "the other is the vanilla mappack's copy");
    free(text);

    check_library(&w, LIB_OFFICIAL_URI, "the library points at the official server again");
    uninstall_report_free(&report);
    free(path);
    world_free(&w);
}

static void test_uninstall_with_no_backups_at_all(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report report;
    char *path;
    size_t i;
    static const char *names[] = { TAB_LEVEL_FILE, TAB_CHALLENGE };

    test_case("...even when there is no backup at all, as an old installer leaves it");
    world_build(&w, "game_old_install");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install first (%s)", err);
    install_report_free(&installed);

    /* Every backup gone: the game now looks exactly as an older installer
     * would have left it, with the tab in place and no copy of anything. */
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        path = backup_path(&w, names[i]);
        plat_remove_file(path);
        free(path);
    }
    world_add_originals(&w, "the vanilla file");

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) == 0,
          "the uninstall still goes ahead (%s)", err);
    CHECK_NUM(report.from_backups, 0, "nothing came from a backup");
    CHECK_NUM(report.from_originals, 2, "both files came from the vanilla mappack");

    for (i = 0; i < sizeof names / sizeof *names; i++) {
        char *text = game_file(&w, names[i]);
        CHECK_STR(text, "the vanilla file", "the game has its own file back");
        free(text);
    }

    uninstall_report_free(&report);
    world_free(&w);
}

/* Points the library at `uri` without going through an install. */
static void patch_library(world *w, const char *uri)
{
    char err[TB_ERR_LEN];
    str_list known = {0};
    lib_image img;
    config *cfg = config_load(err, sizeof err);

    server_known_uris(cfg, w->dig, &known);
    if (lib_open(&w->paths, &known, &img, err, sizeof err) == 0) {
        CHECK(lib_write_uri(&img, uri, err, sizeof err) == 0,
              "the library can be patched by hand (%s)", err);
        lib_close(&img);
    } else {
        CHECK(0, "the library opens (%s)", err);
    }
    str_list_free(&known);
    config_free(cfg);
}

/*
 * The whole point of the fallback: a tab that an older installer put in, taken
 * out by tabber. Nothing of that install is ours — no backups, no state file
 * entry — and the only evidence it happened at all is the patched library.
 */
static void test_uninstall_an_old_installers_work(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    uninstall_report report;
    lib_health health;
    config *cfg;
    char *path, *text;

    test_case("a tab an older installer put in can be taken out by this one");
    world_build(&w, "game_old_installer");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* The game as that installer leaves it: its files in place, no backup of
     * anything, the library redirected, and the state file none the wiser. */
    path = path_join(w.levels, TAB_LEVEL_FILE);
    test_write(path, TAB_LEVEL_BODY);
    free(path);
    path = path_join(w.levels, TAB_CHALLENGE);
    test_write(path, TAB_CHALLENGE_BODY);
    free(path);
    patch_library(&w, EXPECTED_PATCH);
    world_add_originals(&w, "the vanilla file");

    /* That state is recognised rather than reported as damage. */
    cfg = config_load(err, sizeof err);
    lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err);
    CHECK(health.healthy, "the game passes the library check (%s)", health.detail);
    CHECK(health.unrecorded, "as an install nothing here recorded");
    config_free(cfg);

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) == 0,
          "and it uninstalls (%s)", err);
    CHECK_NUM(report.from_backups, 0, "there was no backup to restore from");
    CHECK_NUM(report.from_originals, 2, "so both files came from the vanilla mappack");

    text = game_file(&w, TAB_LEVEL_FILE);
    CHECK_STR(text, "the vanilla file", "the level file is the game's own again");
    free(text);
    text = game_file(&w, TAB_CHALLENGE);
    CHECK_STR(text, "the vanilla file", "and so is the challenge file");
    free(text);
    check_library(&w, LIB_OFFICIAL_URI, "the library points at the official server again");

    /* And now tabber knows about the tab, which it did not before. */
    cfg = config_load(err, sizeof err);
    CHECK(config_find_tab(cfg, TAB_CODE) != NULL, "the tab has a state entry now");
    lib_check(cfg, w.dig, &w.paths, &health, err, sizeof err);
    CHECK(health.healthy && !health.unrecorded, "and the game is plainly clean");
    config_free(cfg);

    uninstall_report_free(&report);
    world_free(&w);
}

/* What the library's credit region currently reads. Caller frees. */
static char *library_credit(world *w)
{
    size_t len = 0;
    char *data = (char *)test_read_bytes(w->library, &len);
    char *found = NULL;
    size_t i;

    /* The credit sits between a NUL and its own padding, so the text right
     * after the URI's terminator is it. */
    for (i = 0; data && i + 1 < len; i++) {
        if (data[i] == 0 && data[i + 1] != 0 && data[i + 1] != 'A' && data[i + 1] != 'B') {
            found = str_dup(data + i + 1);
            break;
        }
    }
    free(data);
    return found;
}

/*
 * The older installers also replaced the developer credit the game renders
 * with the tab's author, so tabber does the same and puts it back.
 */
/*
 * The credit can only be found by what it says, so the guards that keep a
 * chance match from being written over are the whole safety of it.
 */
static void test_credit_refusals(void)
{
    char err[TB_ERR_LEN];
    char *dir = test_dir("credit_guards");
    char *path = path_join(dir, "fake.dll");
    lib_image img;
    byte_buf blob = {0};
    size_t i;

    test_case("a credit that cannot be told apart is not written over");

    /* Filler, then "Nobody" twice: which one is the credit is unknowable. */
    for (i = 0; i < 32; i++)
        buf_append(&blob, "A", 1);
    buf_append(&blob, "\0", 1);
    buf_append(&blob, "Nobody", 6);
    for (i = 0; i < 16; i++)
        buf_append(&blob, "\0", 1);
    buf_append(&blob, "Nobody", 6);
    for (i = 0; i < 16; i++)
        buf_append(&blob, "\0", 1);

    memset(&img, 0, sizeof img);
    img.path = path;
    img.size = blob.len;
    img.data = buf_finish(&blob, NULL);
    test_write_bytes(path, img.data, img.size);

    CHECK(lib_write_credit(&img, "Nobody", "Someone", err, sizeof err) != 0,
          "two of them is one too many");
    CHECK(strstr(err, "2 times") != NULL, "and it says how many (%s)", err);
    free(img.data);

    /* One occurrence, but in the middle of something else: no NUL in front. */
    memset(&blob, 0, sizeof blob);
    for (i = 0; i < 32; i++)
        buf_append(&blob, "A", 1);
    buf_append(&blob, "Nobody", 6);
    for (i = 0; i < 16; i++)
        buf_append(&blob, "\0", 1);
    img.size = blob.len;
    img.data = buf_finish(&blob, NULL);
    test_write_bytes(path, img.data, img.size);

    CHECK(lib_write_credit(&img, "Nobody", "Someone", err, sizeof err) != 0,
          "a match inside another string is refused");
    CHECK(strstr(err, "part of") != NULL, "and it says why (%s)", err);
    free(img.data);

    /* A NUL in front, but a neighbour close behind that we would clobber. */
    memset(&blob, 0, sizeof blob);
    for (i = 0; i < 32; i++)
        buf_append(&blob, "A", 1);
    buf_append(&blob, "\0", 1);
    buf_append(&blob, "Nobody", 6);
    buf_append(&blob, "\0", 1);
    buf_append(&blob, "and the next string", 19);
    buf_append(&blob, "\0", 1);
    img.size = blob.len;
    img.data = buf_finish(&blob, NULL);
    test_write_bytes(path, img.data, img.size);

    CHECK(lib_write_credit(&img, "Nobody", "Someone", err, sizeof err) != 0,
          "a neighbour inside the region is refused");
    CHECK(strstr(err, "overwritten") != NULL, "and it says why (%s)", err);
    free(img.data);

    free(path);
    free(dir);
}

static void test_credit_line(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    install_report installed;
    uninstall_report removed;
    char *text;

    test_case("the game's credit line names the tab's author while it is installed");
    world_build(&w, "game_credit");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    text = library_credit(&w);
    CHECK_STR(text, LIB_CREDIT_ORIGINAL, "a clean game credits its own developer");
    free(text);

    CHECK(tab_install(w.dig, tab, &w.paths, NULL, &installed, err, sizeof err) == 0,
          "install succeeds (%s)", err);
    CHECK_STR(installed.credit, "Nobody", "the report names the credit written");
    text = library_credit(&w);
    CHECK_STR(text, "Nobody", "and the library carries the tab's author");
    free(text);

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &removed, err, sizeof err) == 0,
          "uninstall succeeds (%s)", err);
    CHECK(removed.credit_restored, "the credit is reported as put back");
    text = library_credit(&w);
    CHECK_STR(text, LIB_CREDIT_ORIGINAL, "and the developer is credited again");
    free(text);

    /* Byte for byte: the padding behind a shorter author has to go back too. */
    CHECK(test_files_equal(w.library, w.pristine_library),
          "the library is the file it was before any of it");

    install_report_free(&installed);
    uninstall_report_free(&removed);
    world_free(&w);
}

static void test_credit_of_an_old_installers_work(void)
{
    char err[TB_ERR_LEN];
    world w;
    const npp_tab *tab;
    uninstall_report report;
    char *text;

    test_case("...and one an older installer wrote is put back too");
    world_build(&w, "game_credit_old");
    tab = world_tab(&w, TAB_CODE);
    if (!tab || !w.dig) { world_free(&w); return; }

    /* The game as an older installer leaves it: files in place, library
     * patched, credit replaced, and nothing recorded. */
    {
        char *path = path_join(w.levels, TAB_LEVEL_FILE);
        test_write(path, TAB_LEVEL_BODY);
        free(path);
        path = path_join(w.levels, TAB_CHALLENGE);
        test_write(path, TAB_CHALLENGE_BODY);
        free(path);
    }
    patch_library(&w, EXPECTED_PATCH);
    {
        char err2[TB_ERR_LEN];
        str_list known = {0};
        lib_image img;
        config *cfg = config_load(err2, sizeof err2);

        server_known_uris(cfg, w.dig, &known);
        if (lib_open(&w.paths, &known, &img, err2, sizeof err2) == 0) {
            CHECK(lib_write_credit(&img, LIB_CREDIT_ORIGINAL, "Nobody",
                                   err2, sizeof err2) == 0,
                  "the credit can be replaced by hand (%s)", err2);
            lib_close(&img);
        }
        str_list_free(&known);
        config_free(cfg);
    }
    world_add_originals(&w, "the vanilla file");

    CHECK(tab_uninstall(w.dig, tab, &w.paths, NULL, &report, err, sizeof err) == 0,
          "the uninstall goes ahead (%s)", err);
    CHECK(report.credit_restored, "and the credit is put back");
    text = library_credit(&w);
    CHECK_STR(text, LIB_CREDIT_ORIGINAL, "though nothing recorded changing it");
    free(text);

    uninstall_report_free(&report);
    world_free(&w);
}

static void test_credit_text(void)
{
    char *text;

    test_case("an author is trimmed to what the game can draw");

    text = lib_credit_text("Nobody");
    CHECK_STR(text, "Nobody", "plain ASCII comes through as it is");
    free(text);

    /* The real case: the font has no Unicode, so the rest is dropped. */
    /* Split so the hex escape ends where it should: \x eats every hex digit
     * that follows it, and "drive" starts with one. */
    text = lib_credit_text("flux\xcd\xa2\xc9\x95" "drive");
    CHECK_STR(text, "fluxdrive", "anything outside ASCII is dropped");
    free(text);

    text = lib_credit_text("A ridiculously long list of authors");
    CHECK_STR(text, "A ridiculously l", "a long one is cut to the room there is");
    CHECK(text == NULL || strlen(text) <= LIB_CREDIT_BUDGET, "never past the budget");
    free(text);

    /* Cutting can land on a space, which would look like a mistake in game. */
    text = lib_credit_text("Somebody Else's Team");
    CHECK_STR(text, "Somebody Else's", "and a cut that ends on a space loses it");
    free(text);

    CHECK(lib_credit_text("") == NULL, "an empty author is nothing to write");
    CHECK(lib_credit_text(NULL) == NULL, "nor is none at all");
    text = lib_credit_text("\xe4\xb8\x96\xe7\x95\x8c");
    CHECK(text == NULL, "nor one the font cannot draw a single letter of");
    free(text);
}

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
    test_palette_not_ours();
    test_texts_by_language();
    test_texts_from_the_old_installer();
    test_controls_bound_by_the_tab();
    test_controls_without_a_bindings_file();
    test_install_over_a_stale_backup();
    test_uninstall_without_backups();
    test_uninstall_with_no_backups_at_all();
    test_uninstall_an_old_installers_work();
    test_credit_text();
    test_credit_refusals();
    test_credit_line();
    test_credit_of_an_old_installers_work();
    test_codes();
}
