#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloud.h"
#include "config.h"
#include "install.h"
#include "json.h"
#include "keys.h"
#include "loc.h"
#include "palettes.h"
#include "patch.h"
#include "platform.h"
#include "save.h"
#include "server.h"
#include "tabs.h"
#include "util.h"

/* One file on its way from the tab store into the game folder. */
typedef struct {
    char *name;     /* file name, e.g. "SI.txt"            */
    char *source;   /* its copy in the tab store           */
    char *target;   /* the game file it replaces           */
    char *backup;   /* target + "OG", the original set aside */
    char *data;     /* contents, read before any change     */
    size_t len;
    char *original; /* the game's own copy, when no backup was kept */
    size_t original_len;
} install_file;

void install_options_init(install_options *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->save_flags = 0;            /* leave a save in the form it came in  */
    opts->cloud = CLOUD_REPLACE;     /* keep Steam's copy in step with it    */
    opts->palettes = PALETTE_SKIP;   /* never overwrite a palette of theirs  */
    opts->keep_palettes = 0;         /* an uninstall takes them out again    */
    opts->languages = NULL;          /* the texts change in every language   */
}

/*
 * What opts->languages being NULL stands for. Every language, because the
 * replacements say what the panel actually shows and the original no longer
 * does, whichever language the player reads it in.
 */
static const loc_langs loc_langs_every = { LOC_LANGS_ALL, { NULL, 0, 0 } };

/* The options a caller that passed none would have meant. */
static install_options options_or_default(const install_options *opts)
{
    install_options out;

    install_options_init(&out);
    if (opts)
        out = *opts;
    return out;
}

/*
 * Puts the Steam Cloud copies in step with the savefile that was just written.
 * Never fatal: a cloud copy we could not reach is a warning on the report, not
 * a reason to undo an install that is otherwise done.
 */
static void apply_cloud(const npp_paths *paths, const install_options *opts,
                        const save_plan *save, cloud_report *report, char *warning,
                        size_t warnsz)
{
    char err[TB_ERR_LEN];

    if (!save->save || !save->save_len)
        return;
    if (cloud_apply(paths, opts->cloud, save->save, save->save_len,
                    report, err, sizeof err) != 0)
        err_set(warning, warnsz, "the Steam Cloud copies were left as they were: %s", err);
}

/* ---- Detection --------------------------------------------------------- */

int install_detect(config *cfg, const npp_paths *paths, char *code_out, size_t code_sz)
{
    const json_value *tabs = json_get(cfg->root, CJK_TABS);
    const json_value *entry;

    (void)paths;   /* reserved for the game-folder evidence mentioned above */

    for (entry = tabs ? tabs->children : NULL; entry; entry = entry->next) {
        if (entry->type != JSON_OBJECT || !json_get_bool(entry, CJK_INSTALLED, 0))
            continue;
        if (code_out)
            snprintf(code_out, code_sz, "%s", json_get_string(entry, CJK_CODE, "?"));
        return 1;
    }

    if (code_out && code_sz)
        code_out[0] = '\0';
    return 0;
}

/* ---- Helpers ----------------------------------------------------------- */

/* Copies the strings of a JSON array into a list. */
static void json_array_to_list(const json_value *array, str_list *out)
{
    const json_value *item;

    if (!array || array->type != JSON_ARRAY)
        return;
    for (item = array->children; item; item = item->next) {
        if (item->type == JSON_STRING)
            str_list_push(out, str_dup(item->string));
    }
}

/* Joins a list into "a, b, c", truncating politely when it is long. */
static char *list_to_text(const str_list *list)
{
    byte_buf text = {0};
    size_t i, shown = list->count < INSTALL_MAX_REPORTED ? list->count : INSTALL_MAX_REPORTED;

    for (i = 0; i < shown; i++) {
        if (i > 0)
            buf_append(&text, ", ", 2);
        buf_append(&text, list->items[i], strlen(list->items[i]));
    }
    if (list->count > shown) {
        char more[32];
        snprintf(more, sizeof more, ", and %u more",
                 (unsigned)(list->count - shown));
        buf_append(&text, more, strlen(more));
    }
    return buf_finish(&text, NULL);
}

/* Confirms the game folder accepts new files before anything is renamed. */
static int dir_is_writable(const char *dir)
{
    char *probe = path_join(dir, INSTALL_PROBE_FILE);
    int ok = plat_write_file(probe, "", 0) == 0;

    if (ok)
        plat_remove_file(probe);
    free(probe);
    return ok;
}

static void install_files_free(install_file *files, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        free(files[i].name);
        free(files[i].source);
        free(files[i].target);
        free(files[i].backup);
        free(files[i].data);
        free(files[i].original);
    }
    free(files);
}

/*
 * Undoes the first `done` replacements: delete the tab's copy, move the
 * original back into place.
 */
static void rollback(install_file *files, size_t done)
{
    size_t i;

    for (i = 0; i < done; i++) {
        plat_remove_file(files[i].target);
        plat_replace_file(files[i].backup, files[i].target);
    }
}

/* ---- Install ----------------------------------------------------------- */

int tab_install(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                const install_options *opts_in, install_report *report,
                char *err, size_t errsz)
{
    install_options opts = options_or_default(opts_in);
    const char *levels_name = digest_levels_dir(dig);
    const json_value *cfg_node = digest_config(dig);
    str_list supported = {0}, entries = {0}, missing = {0}, conflicts = {0};
    str_list blocked = {0}, known = {0};
    install_file *files = NULL;
    size_t count = 0, i, done = 0;
    char *game_dir = NULL, *tab_dir = NULL, *tab_root = NULL, *patch_uri = NULL;
    char cfg_err[TB_ERR_LEN];
    config *state = NULL;
    lib_image img;
    lib_health health;
    server_addr addr;
    server_source source;
    save_plan save = {0};
    palette_plan palettes = {0};
    loc_plan strings = {0};
    keys_plan bindings = {0};
    int lib_loaded = 0, save_planned = 0, palettes_planned = 0, strings_planned = 0;
    int bindings_planned = 0;
    int rc = -1;

    memset(report, 0, sizeof(*report));

    /* --- Is the game in the state we think it is? --- */
    state = config_load(cfg_err, sizeof cfg_err);
    if (!state) {
        err_set(err, errsz, "%s", cfg_err);
        goto done;
    }
    if (lib_check(state, dig, paths, &health, err, errsz) != 0)
        goto done;
    config_save(state, cfg_err, sizeof cfg_err);   /* record what the check saw */
    if (!health.healthy) {
        err_set(err, errsz, "the game is not in a clean state: %s", health.detail);
        goto done;
    }

    /* --- Where the files come from and go to --- */
    game_dir = path_join(paths->install_dir, NPP_ASSETS_SUBDIR);
    {
        char *levels = path_join(game_dir, levels_name);
        free(game_dir);
        game_dir = levels;
    }
    tab_root = tab_dir_path(tab->code);
    tab_dir = path_join(tab_root, levels_name);

    if (!plat_is_dir(tab_dir)) {
        err_set(err, errsz, "'%s' has no '%s' folder; fetch the tab again",
                tab_root, levels_name);
        goto done;
    }
    if (!plat_is_dir(game_dir)) {
        err_set(err, errsz, "the game has no '%s' folder at '%s'", levels_name, game_dir);
        goto done;
    }

    /* --- Which of the tab's files the game actually supports --- */
    json_array_to_list(json_get(cfg_node, DJK_LEVEL_FILES), &supported);
    json_array_to_list(json_get(cfg_node, DJK_CHALLENGE_FILES), &supported);
    if (supported.count == 0) {
        err_set(err, errsz, "the digest lists no supported level or challenge files");
        goto done;
    }

    if (plat_list_dir(tab_dir, &entries) != 0) {
        err_set(err, errsz, "cannot read '%s'", tab_dir);
        goto done;
    }

    files = entries.count ? xmalloc(entries.count * sizeof(*files)) : NULL;
    for (i = 0; i < entries.count; i++) {
        const char *name = entries.items[i];
        char *src_path = path_join(tab_dir, name);

        if (!plat_is_file(src_path)) {     /* subfolders are not ours to install */
            free(src_path);
            continue;
        }
        if (!str_list_contains(&supported, name)) {
            /* Not a file the game reads: leave it behind and say so. */
            str_list_push(&report->skipped, str_dup(name));
            free(src_path);
            continue;
        }

        memset(&files[count], 0, sizeof files[count]);
        files[count].name = str_dup(name);
        files[count].source = src_path;
        files[count].target = path_join(game_dir, name);
        files[count].backup = str_fmt("%s%s", files[count].target, INSTALL_BACKUP_SUFFIX);
        count++;
    }

    if (count == 0) {
        err_set(err, errsz, "the tab ships no level or challenge files the game supports");
        goto done;
    }

    /* --- Checks, all of them, before a single file is touched --- */
    for (i = 0; i < count; i++) {
        if (!plat_is_file(files[i].target))
            str_list_push(&missing, str_dup(files[i].name));
        else if (plat_is_dir(files[i].backup))
            str_list_push(&blocked, str_dup(files[i].name));
        else if (plat_is_file(files[i].backup))
            str_list_push(&conflicts, str_dup(files[i].name));
    }
    if (missing.count > 0) {
        char *names = list_to_text(&missing);
        err_set(err, errsz, "the game is missing %u file(s) this tab replaces: %s",
                (unsigned)missing.count, names);
        free(names);
        goto done;
    }
    if (blocked.count > 0) {
        /* A folder under a backup's name is nobody's doing but the user's,
         * and the rename would fail on it half way through the install. */
        char *names = list_to_text(&blocked);
        err_set(err, errsz, "%u backup name(s) are taken by a folder: %s. "
                            "Move those '%s' folders out of the game before installing",
                (unsigned)blocked.count, names, INSTALL_BACKUP_SUFFIX);
        free(names);
        goto done;
    }
    if (!dir_is_writable(game_dir)) {
        err_set(err, errsz, "cannot write to '%s'; check the folder's permissions "
                            "and that the game is not running", game_dir);
        goto done;
    }

    /* --- The library must still carry the official URI, exactly once --- */
    server_known_uris(state, dig, &known);
    if (lib_open(paths, &known, &img, err, errsz) != 0)
        goto done;
    lib_loaded = 1;

    if (img.state == LIB_PATCHED) {
        err_set(err, errsz, "the library already points at '%s'; a custom tab "
                            "appears to be installed", img.uri);
        goto done;
    }
    if (img.state != LIB_ORIGINAL) {
        err_set(err, errsz, "the official server URI is not in '%s'; the library "
                            "may have been patched by another tool", img.path);
        goto done;
    }
    if (img.occurrences != 1) {
        err_set(err, errsz, "the official server URI appears %d times in the library, "
                            "expected exactly once", img.occurrences);
        goto done;
    }

    /*
     * The library says no tab is installed, so any backup already sitting in
     * the folder is a leftover — from an install that an older installer, or
     * a hand, undid without clearing it — and the file beside it is the
     * game's own. Overwriting it is right, and saying so is only fair.
     */
    report->stale_backups = conflicts;
    memset(&conflicts, 0, sizeof conflicts);

    /* The address to point the game at, and the URI that will replace the
     * official one. Both have to be settled before anything is written. */
    server_resolve(state, dig, 1, &addr, &source);
    patch_uri = lib_build_uri(&addr, tab->code, LIB_URI_BUDGET, err, errsz);
    if (!patch_uri)
        goto done;

    /* Is that server actually up? Purely diagnostic: a server down for a few
     * minutes of maintenance is no reason to refuse an install, so the verdict
     * is only reported. This is the last look outwards before we write. */
    server_probe(&addr, source, &report->health);

    /* --- The savefile swap, worked out in full before anything is written --- */
    if (save_plan_build(paths, tab->code, 1, opts.save_flags, &save, err, errsz) != 0)
        goto done;
    save_planned = 1;

    /* --- And the palettes: which names are free, and whether they fit --- */
    if (palettes_plan_build(dig, tab, paths, opts.palettes, &palettes, err, errsz) != 0)
        goto done;
    palettes_planned = 1;

    /* --- And the game's own texts, with the originals kept to put back --- */
    if (loc_plan_build(paths, tab, opts.languages ? opts.languages : &loc_langs_every,
                       &strings, err, errsz) != 0)
        goto done;
    strings_planned = 1;

    /*
     * --- And the controls, when the tab's digest entry asks for several
     * players on one set of keys. Unlike everything above, a bindings file we
     * cannot make sense of does not stop the install: the tab still plays,
     * only single-handed co-op does not, and `bind` can put that right once
     * the game has written the file. So it is reported and skipped.
     */
    {
        int players[KEYS_PLAYER_MAX];
        size_t players_count = 0;

        if (keys_players_wanted(tab, players, &players_count, cfg_err, sizeof cfg_err) != 0 ||
            (players_count >= 2 &&
             keys_bind_build(paths, players, players_count, config_get_keybindings(state),
                             &bindings, cfg_err, sizeof cfg_err) != 0))
            err_set(report->warning, sizeof report->warning,
                    "the tab wants several players to share one set of controls, but "
                    "they were left as they are: %s", cfg_err);
        else if (players_count >= 2)
            bindings_planned = 1;
    }

    /* Read everything up front, so the writing phase is as short as possible. */
    for (i = 0; i < count; i++) {
        files[i].data = plat_read_file(files[i].source, &files[i].len);
        if (!files[i].data) {
            err_set(err, errsz, "cannot read '%s'", files[i].source);
            goto done;
        }
    }

    /* --- Apply: set the original aside, put the tab's copy in its place --- */
    for (i = 0; i < count; i++) {
        if (plat_replace_file(files[i].target, files[i].backup) != 0) {
            err_set(err, errsz, "cannot rename '%s' to '%s'; is the game running?",
                    files[i].target, files[i].backup);
            rollback(files, done);
            goto done;
        }
        if (plat_write_file(files[i].target, files[i].data, files[i].len) != 0) {
            err_set(err, errsz, "cannot write '%s'", files[i].target);
            plat_remove_file(files[i].target);
            plat_replace_file(files[i].backup, files[i].target);   /* undo this one */
            rollback(files, done);
            goto done;
        }
        done++;
    }

    /* --- The palettes, copied into the game's own folder --- */
    if (palettes_plan_apply(&palettes, &report->palettes, err, errsz) != 0) {
        rollback(files, done);
        goto done;
    }

    /* --- The game's own texts, rewritten in one pass over loc.txt --- */
    if (loc_plan_apply(&strings, &report->strings, err, errsz) != 0) {
        palettes_plan_undo(&palettes);
        rollback(files, done);
        goto done;
    }

    /* --- The controls, so one player can drive two ninjas --- */
    if (bindings_planned &&
        keys_plan_apply(&bindings, &report->bindings, err, errsz) != 0) {
        loc_plan_undo(&strings);
        palettes_plan_undo(&palettes);
        rollback(files, done);
        goto done;
    }

    /* --- Redirect the game's queries, last and quickest --- */
    if (lib_write_uri(&img, patch_uri, err, errsz) != 0) {
        keys_plan_undo(&bindings);
        loc_plan_undo(&strings);
        palettes_plan_undo(&palettes);
        rollback(files, done);        /* the level files go back as they were */
        goto done;
    }

    /* --- And the savefile: the vanilla one filed away, the tab's put in --- */
    if (save_plan_apply(&save, &report->save, err, errsz) != 0) {
        lib_write_uri(&img, LIB_OFFICIAL_URI, cfg_err, sizeof cfg_err);
        keys_plan_undo(&bindings);
        loc_plan_undo(&strings);
        palettes_plan_undo(&palettes);
        rollback(files, done);
        goto done;
    }

    /* Steam keeps its own copy of the savefile and prefers it to the local
     * one, so it has to be dealt with or it would undo the swap above. */
    apply_cloud(paths, &opts, &save, &report->cloud, report->warning,
                sizeof report->warning);

    report->installed_count = done;
    report->game_levels_dir = game_dir;
    report->tab_levels_dir = tab_dir;
    game_dir = tab_dir = NULL;               /* ownership moves to the report */
    snprintf(report->server_uri, sizeof report->server_uri, "%s", patch_uri);
    snprintf(report->server_source, sizeof report->server_source, "%s",
             server_source_name(source));

    /* Nothing can fail from here on, so the palettes we replaced can go. */
    palettes_plan_commit(&palettes);

    /* --- Record it --- */
    config_set_installed(state, tab->id, tab->code);
    config_set_state_library(state, 1);      /* the library now matches */
    {
        /* Only the folders we really created, so an uninstall deletes ours
         * and leaves alone the ones that were already there. */
        str_list made = {0};

        palettes_plan_installed(&palettes, &made);
        config_set_palettes(state, tab->id, tab->code, &made);
        str_list_free(&made);
    }
    /* The texts we overwrote, so an uninstall can put them back without
     * tabber having to carry a copy of the game's own strings. */
    config_set_strings(state, loc_plan_take_record(&strings));
    /* The bindings likewise, merged with any an earlier `bind` recorded. */
    if (bindings_planned)
        config_set_keybindings(state, keys_plan_take_record(&bindings));
    if (config_save(state, cfg_err, sizeof cfg_err) != 0)
        err_set(report->warning, sizeof report->warning,
                "the install was not recorded: %s", cfg_err);
    else
        snprintf(report->state_path, sizeof report->state_path, "%s", state->path);
    rc = 0;

done:
    if (lib_loaded)
        lib_close(&img);
    if (save_planned)
        save_plan_free(&save);
    if (palettes_planned)
        palettes_plan_free(&palettes);
    if (strings_planned)
        loc_plan_free(&strings);
    if (bindings_planned)
        keys_plan_free(&bindings);
    if (state)
        config_free(state);
    install_files_free(files, count);
    str_list_free(&supported);
    str_list_free(&entries);
    str_list_free(&missing);
    str_list_free(&conflicts);
    str_list_free(&blocked);
    str_list_free(&known);
    free(patch_uri);
    free(game_dir);
    free(tab_dir);
    free(tab_root);
    return rc;
}

void install_report_free(install_report *report)
{
    str_list_free(&report->stale_backups);
    cloud_report_free(&report->cloud);
    palette_report_free(&report->palettes);
    loc_report_free(&report->strings);
    keys_report_free(&report->bindings);
    free(report->game_levels_dir);
    free(report->tab_levels_dir);
    str_list_free(&report->skipped);
    report->game_levels_dir = report->tab_levels_dir = NULL;
}

/* ---- Uninstall --------------------------------------------------------- */

/*
 * The files this tab ships according to the digest, split into the ones the
 * game reads and the ones it does not. Taking the list from the digest instead
 * of the tab folder means an uninstall still works once the download is gone.
 */
static void collect_shipped_files(const digest *dig, const npp_tab *tab,
                                  str_list *wanted, str_list *skipped)
{
    const json_value *cfg_node = digest_config(dig);
    const json_value *disk = json_get(tab->node, TJK_DISK);
    str_list supported = {0}, shipped = {0};
    size_t i;

    json_array_to_list(json_get(cfg_node, DJK_LEVEL_FILES), &supported);
    json_array_to_list(json_get(cfg_node, DJK_CHALLENGE_FILES), &supported);
    json_array_to_list(json_get(disk, TJK_LEVEL_FILES), &shipped);
    json_array_to_list(json_get(disk, TJK_CHALLENGE_FILES), &shipped);

    for (i = 0; i < shipped.count; i++) {
        if (str_list_contains(&supported, shipped.items[i]))
            str_list_push(wanted, str_dup(shipped.items[i]));
        else
            str_list_push(skipped, str_dup(shipped.items[i]));
    }

    str_list_free(&supported);
    str_list_free(&shipped);
}

/*
 * Collects backups left in the game folder that this uninstall did not handle.
 * They are a sign of drift (an older install, a tab whose file list changed)
 * and worth telling the user about, without failing the uninstall.
 */
static void collect_leftover_backups(const char *game_dir, const str_list *handled,
                                     str_list *leftovers)
{
    str_list entries = {0};
    size_t i, suffix_len = sizeof(INSTALL_BACKUP_SUFFIX) - 1;

    if (plat_list_dir(game_dir, &entries) != 0)
        return;

    for (i = 0; i < entries.count; i++) {
        const char *name = entries.items[i];
        size_t len = strlen(name);
        char *original;

        if (len <= suffix_len || strcmp(name + len - suffix_len, INSTALL_BACKUP_SUFFIX) != 0)
            continue;
        original = str_fmt("%.*s", (int)(len - suffix_len), name);
        if (!str_list_contains(handled, original))
            str_list_push(leftovers, original);
        else
            free(original);
    }

    str_list_free(&entries);
}

/*
 * Reads the game's own copies of the files that have no "OG" backup. Those are
 * the ones an installer that predates tabber replaced without keeping a copy of
 * anything; the first mappack in the digest *is* the vanilla game, so its files
 * are the originals, and it is downloaded here if the store does not have it
 * already. Returns 0 with `files[i].original` filled in for every file that
 * needs it, or -1 with a reason in `err` and nothing changed on disk.
 */
static int load_originals(const digest *dig, install_file *files, size_t count,
                          size_t wanted, uninstall_report *report,
                          char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];
    const npp_tab *originals = digest_find(dig, INSTALL_ORIGINALS_CODE);
    char *root, *dir;
    size_t i;
    int rc = -1;

    if (!originals) {
        err_set(err, errsz, "%u of the tab's file(s) have no '%s' backup, and the "
                            "digest has no '%s' mappack to take the originals from",
                (unsigned)wanted, INSTALL_BACKUP_SUFFIX, INSTALL_ORIGINALS_CODE);
        return -1;
    }
    snprintf(report->originals_code, sizeof report->originals_code, "%s",
             originals->code);

    if (!tab_is_downloaded(originals->code)) {
        tab_report fetched;

        if (tab_fetch(dig, originals, &fetched, sub, sizeof sub) != 0) {
            err_set(err, errsz, "%u of the tab's file(s) have no '%s' backup, and the "
                                "game's own copies could not be downloaded: %s",
                    (unsigned)wanted, INSTALL_BACKUP_SUFFIX, sub);
            return -1;
        }
        tab_report_free(&fetched);
        report->fetched_originals = 1;
    }

    root = tab_dir_path(originals->code);
    dir = path_join(root, digest_levels_dir(dig));
    for (i = 0; i < count; i++) {
        char *source;

        if (plat_is_file(files[i].backup))
            continue;                    /* its own backup is right there */
        source = path_join(dir, files[i].name);
        files[i].original = plat_read_file(source, &files[i].original_len);
        if (!files[i].original) {
            err_set(err, errsz, "'%s' has no '%s' backup, and the '%s' mappack has no "
                                "copy of it either at '%s'", files[i].name,
                    INSTALL_BACKUP_SUFFIX, originals->code, source);
            free(source);
            goto done;
        }
        free(source);
    }
    rc = 0;

done:
    free(dir);
    free(root);
    return rc;
}

int tab_uninstall(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                  const install_options *opts_in, uninstall_report *report,
                  char *err, size_t errsz)
{
    install_options opts = options_or_default(opts_in);
    const char *levels_name = digest_levels_dir(dig);
    str_list wanted = {0}, missing = {0}, no_backup = {0}, known = {0};
    install_file *files = NULL;
    size_t count = 0, i, restored = 0;
    char *game_dir = NULL;          /* the checks above may bail out first */
    char cfg_err[TB_ERR_LEN];
    char patched_uri[LIB_URI_MAX];
    config *state = NULL;
    lib_image img;
    lib_health health;
    save_plan save = {0};
    loc_plan strings = {0};
    keys_plan bindings = {0};
    str_list palettes = {0};
    int lib_loaded = 0, save_planned = 0, strings_planned = 0, strings_done = 0;
    int bindings_planned = 0, bindings_done = 0;
    int rc = -1;

    memset(report, 0, sizeof(*report));
    patched_uri[0] = '\0';

    /* --- Is the game in the state we think it is? --- */
    state = config_load(cfg_err, sizeof cfg_err);
    if (!state) {
        err_set(err, errsz, "%s", cfg_err);
        goto done;
    }
    if (lib_check(state, dig, paths, &health, err, errsz) != 0)
        goto done;
    config_save(state, cfg_err, sizeof cfg_err);   /* record what the check saw */
    if (!health.healthy) {
        err_set(err, errsz, "the game is not in a clean state: %s", health.detail);
        goto done;
    }

    game_dir = path_join(paths->install_dir, NPP_ASSETS_SUBDIR);
    {
        char *levels = path_join(game_dir, levels_name);
        free(game_dir);
        game_dir = levels;
    }
    if (!plat_is_dir(game_dir)) {
        err_set(err, errsz, "the game has no '%s' folder at '%s'", levels_name, game_dir);
        goto done;
    }

    /*
     * The palettes to take out, as recorded when they went in. An empty record
     * means the install put none in, and nothing is to be deleted; no record
     * at all means the install predates this, and the digest's own list is
     * then the best guess we have.
     */
    if (!config_get_palettes(state, tab->code, &palettes))
        palettes_bundled(tab, &palettes);

    collect_shipped_files(dig, tab, &wanted, &report->skipped);
    if (wanted.count == 0) {
        err_set(err, errsz, "the digest lists no installable files for this tab");
        goto done;
    }

    files = xmalloc(wanted.count * sizeof(*files));
    for (i = 0; i < wanted.count; i++) {
        memset(&files[count], 0, sizeof files[count]);
        files[count].name = str_dup(wanted.items[i]);
        files[count].target = path_join(game_dir, wanted.items[i]);
        files[count].backup = str_fmt("%s%s", files[count].target, INSTALL_BACKUP_SUFFIX);
        count++;
    }

    /* --- Checks, all of them, before anything moves --- */
    for (i = 0; i < count; i++) {
        if (!plat_is_file(files[i].target))
            str_list_push(&missing, str_dup(files[i].name));
        if (!plat_is_file(files[i].backup))
            str_list_push(&no_backup, str_dup(files[i].name));
    }
    if (missing.count > 0) {
        char *names = list_to_text(&missing);
        err_set(err, errsz, "%u of the tab's file(s) are not in the game folder: %s. "
                            "Is this tab really installed?",
                (unsigned)missing.count, names);
        free(names);
        goto done;
    }
    if (!dir_is_writable(game_dir)) {
        err_set(err, errsz, "cannot write to '%s'; check the folder's permissions "
                            "and that the game is not running", game_dir);
        goto done;
    }

    /* --- The library must point at this very tab --- */
    server_known_uris(state, dig, &known);
    if (lib_open(paths, &known, &img, err, errsz) != 0)
        goto done;
    lib_loaded = 1;

    if (img.state == LIB_ORIGINAL) {
        err_set(err, errsz, "the library still points at the official server, so no "
                            "custom tab appears to be installed");
        goto done;
    }
    if (img.state != LIB_PATCHED) {
        err_set(err, errsz, "the library points at no URI we recognise; it may have "
                            "been patched by another tool");
        goto done;
    }
    if (img.occurrences != 1) {
        err_set(err, errsz, "the server URI appears %d times in the library, expected "
                            "exactly once", img.occurrences);
        goto done;
    }
    if (!str_ieq(img.code, tab->code)) {
        /* The library names a different tab than the one being uninstalled:
         * whatever the state file says, the game is not consistent. */
        err_set(err, errsz, "the library is patched for '%s', not '%s'. The game is in "
                            "an inconsistent state: what is installed is not what we "
                            "were asked to remove",
                img.code[0] ? img.code : "an unnamed tab", tab->code);
        goto done;
    }
    snprintf(patched_uri, sizeof patched_uri, "%s", img.uri);

    /*
     * --- The originals that were never backed up, from the vanilla mappack.
     * This is the one step that may reach the network, and it happens here:
     * after the library has confirmed that this tab really is the one that is
     * installed, and before a single file is written.
     */
    if (no_backup.count > 0 &&
        load_originals(dig, files, count, no_backup.count, report, err, errsz) != 0)
        goto done;

    /* --- The savefile swap, worked out before anything is written --- */
    if (save_plan_build(paths, tab->code, 0, opts.save_flags, &save, err, errsz) != 0)
        goto done;
    save_planned = 1;

    /*
     * --- And the texts to put back. What they were is read from the state
     * file, so nothing about the game's own strings is hardcoded; a record
     * that is not there still leaves the three English ones the old installer
     * overwrote, which are. Failing to work that out is not a reason to refuse
     * to uninstall, so it is reported and the rest goes ahead.
     */
    if (loc_restore_build(paths, config_get_strings(state), &strings,
                          cfg_err, sizeof cfg_err) != 0)
        err_set(report->warning, sizeof report->warning,
                "the game's texts were left as they are: %s", cfg_err);
    else
        strings_planned = 1;

    /*
     * --- And the controls, put back the same way and from the same kind of
     * record. Nothing on record means nothing was ever bound, and the
     * bindings file is then not even opened: a player who never ran the game
     * has none, and that is no reason to complain.
     */
    if (json_count(config_get_keybindings(state)) > 0) {
        if (keys_unbind_build(paths, NULL, 0, config_get_keybindings(state), &bindings,
                              cfg_err, sizeof cfg_err) != 0)
            err_set(report->warning, sizeof report->warning,
                    "the game's key bindings were left as they are: %s", cfg_err);
        else
            bindings_planned = 1;
    }

    /* Keep the tab's copies in memory, so a failure part-way can be undone. */
    for (i = 0; i < count; i++) {
        files[i].data = plat_read_file(files[i].target, &files[i].len);
        if (!files[i].data) {
            err_set(err, errsz, "cannot read '%s'", files[i].target);
            goto done;
        }
    }

    /*
     * --- Apply, undoing the install in reverse order: the savefile first,
     * then the library, then the level files ---
     */
    if (save_plan_apply(&save, &report->save, err, errsz) != 0)
        goto done;

    apply_cloud(paths, &opts, &save, &report->cloud, report->warning,
                sizeof report->warning);

    if (lib_write_uri(&img, LIB_OFFICIAL_URI, err, errsz) != 0) {
        save_plan_undo(&save);
        goto done;
    }

    for (i = 0; i < count; i++) {
        int ok;

        if (files[i].original) {
            /* Nothing was set aside for this one, so the game's own copy is
             * written straight over the tab's, as the old installers did. */
            ok = plat_write_file(files[i].target, files[i].original,
                                 files[i].original_len) == 0;
            if (ok)
                report->from_originals++;
        } else {
            ok = plat_replace_file(files[i].backup, files[i].target) == 0;
            if (ok)
                report->from_backups++;
        }

        if (!ok) {
            err_set(err, errsz, "cannot restore '%s'; is the game running?",
                    files[i].target);
            /* Put back what was already restored: move the original aside
             * again where there was one, rewrite the tab's copy, and re-point
             * the library. */
            while (restored-- > 0) {
                if (!files[restored].original)
                    plat_replace_file(files[restored].target, files[restored].backup);
                plat_write_file(files[restored].target, files[restored].data,
                                files[restored].len);
            }
            report->from_backups = report->from_originals = 0;
            lib_write_uri(&img, patched_uri, cfg_err, sizeof cfg_err);
            save_plan_undo(&save);
            goto done;
        }
        restored++;
    }

    /*
     * The texts and then the palettes, last: the game is already back as it
     * was, so neither is a reason to undo any of it if it will not go through.
     */
    if (strings_planned) {
        if (loc_plan_apply(&strings, &report->strings, cfg_err, sizeof cfg_err) != 0)
            err_set(report->warning, sizeof report->warning,
                    "the game's texts were left as they are: %s", cfg_err);
        else
            strings_done = 1;
    }

    if (bindings_planned) {
        if (keys_plan_apply(&bindings, &report->bindings, cfg_err, sizeof cfg_err) != 0)
            err_set(report->warning, sizeof report->warning,
                    "the game's key bindings were left as they are: %s", cfg_err);
        else
            bindings_done = 1;
    }

    if (palettes_remove(dig, paths, &palettes, opts.keep_palettes, &report->palettes,
                        cfg_err, sizeof cfg_err) != 0)
        err_set(report->warning, sizeof report->warning,
                "the tab's palettes were left in the game folder: %s", cfg_err);

    report->restored_count = restored;
    report->game_levels_dir = game_dir;
    collect_leftover_backups(report->game_levels_dir, &wanted, &report->leftovers);
    game_dir = NULL;                 /* ownership moves to the report */
    snprintf(report->server_uri, sizeof report->server_uri, "%s", LIB_OFFICIAL_URI);

    /* --- Record it. install_date is left alone: it is still true. --- */
    config_set_uninstalled(state, tab->id, tab->code);
    if (strings_done)
        config_set_strings(state, NULL);    /* the originals are back in place */
    if (bindings_done)
        config_set_keybindings(state, keys_plan_take_record(&bindings));  /* likewise */
    if (!opts.keep_palettes && report->palettes.failed == 0)
        config_set_palettes(state, tab->id, tab->code, NULL);   /* they are gone */
    config_set_state_library(state, 1);      /* the library is official again */
    if (config_save(state, cfg_err, sizeof cfg_err) != 0)
        err_set(report->warning, sizeof report->warning,
                "the uninstall was not recorded: %s", cfg_err);
    else
        snprintf(report->state_path, sizeof report->state_path, "%s", state->path);
    rc = 0;

done:
    if (lib_loaded)
        lib_close(&img);
    if (save_planned)
        save_plan_free(&save);
    if (strings_planned)
        loc_plan_free(&strings);
    if (bindings_planned)
        keys_plan_free(&bindings);
    if (state)
        config_free(state);
    install_files_free(files, count);
    str_list_free(&palettes);
    str_list_free(&wanted);
    str_list_free(&missing);
    str_list_free(&no_backup);
    str_list_free(&known);
    free(game_dir);
    return rc;
}

void uninstall_report_free(uninstall_report *report)
{
    cloud_report_free(&report->cloud);
    palette_report_free(&report->palettes);
    loc_report_free(&report->strings);
    keys_report_free(&report->bindings);
    free(report->game_levels_dir);
    str_list_free(&report->skipped);
    str_list_free(&report->leftovers);
    report->game_levels_dir = NULL;
}
