#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "install.h"
#include "json.h"
#include "patch.h"
#include "platform.h"
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
} install_file;

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
                install_report *report, char *err, size_t errsz)
{
    const char *levels_name = digest_levels_dir(dig);
    const json_value *cfg_node = digest_config(dig);
    str_list supported = {0}, entries = {0}, missing = {0}, conflicts = {0};
    str_list known = {0};
    install_file *files = NULL;
    size_t count = 0, i, done = 0;
    char *game_dir = NULL, *tab_dir = NULL, *tab_root = NULL, *patch_uri = NULL;
    char cfg_err[TB_ERR_LEN];
    config *state = NULL;
    lib_image img;
    lib_health health;
    server_addr addr;
    server_source source;
    int lib_loaded = 0, rc = -1;

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

        files[count].name = str_dup(name);
        files[count].source = src_path;
        files[count].target = path_join(game_dir, name);
        files[count].backup = str_fmt("%s%s", files[count].target, INSTALL_BACKUP_SUFFIX);
        files[count].data = NULL;
        files[count].len = 0;
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
        else if (plat_is_file(files[i].backup) || plat_is_dir(files[i].backup))
            str_list_push(&conflicts, str_dup(files[i].name));
    }
    if (missing.count > 0) {
        char *names = list_to_text(&missing);
        err_set(err, errsz, "the game is missing %u file(s) this tab replaces: %s",
                (unsigned)missing.count, names);
        free(names);
        goto done;
    }
    if (conflicts.count > 0) {
        char *names = list_to_text(&conflicts);
        err_set(err, errsz, "backups already exist for %u file(s): %s. "
                            "A previous installation was not undone; restore those "
                            "'%s' files before installing",
                (unsigned)conflicts.count, names, INSTALL_BACKUP_SUFFIX);
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

    /* The address to point the game at, and the URI that will replace the
     * official one. Both have to be settled before anything is written. */
    server_resolve(state, dig, 1, &addr, &source);
    patch_uri = lib_build_uri(&addr, tab->code, LIB_URI_BUDGET, err, errsz);
    if (!patch_uri)
        goto done;

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

    /* --- Redirect the game's queries, last and quickest --- */
    if (lib_write_uri(&img, patch_uri, err, errsz) != 0) {
        rollback(files, done);        /* the level files go back as they were */
        goto done;
    }

    report->installed_count = done;
    report->game_levels_dir = game_dir;
    report->tab_levels_dir = tab_dir;
    game_dir = tab_dir = NULL;               /* ownership moves to the report */
    snprintf(report->server_uri, sizeof report->server_uri, "%s", patch_uri);
    snprintf(report->server_source, sizeof report->server_source, "%s",
             server_source_name(source));

    /* --- Record it --- */
    config_set_installed(state, tab->id, tab->code);
    config_set_state_library(state, 1);      /* the library now matches */
    if (config_save(state, cfg_err, sizeof cfg_err) != 0)
        err_set(report->warning, sizeof report->warning,
                "the install was not recorded: %s", cfg_err);
    else
        snprintf(report->state_path, sizeof report->state_path, "%s", state->path);
    rc = 0;

done:
    if (lib_loaded)
        lib_close(&img);
    if (state)
        config_free(state);
    install_files_free(files, count);
    str_list_free(&supported);
    str_list_free(&entries);
    str_list_free(&missing);
    str_list_free(&conflicts);
    str_list_free(&known);
    free(patch_uri);
    free(game_dir);
    free(tab_dir);
    free(tab_root);
    return rc;
}

void install_report_free(install_report *report)
{
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

int tab_uninstall(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                  uninstall_report *report, char *err, size_t errsz)
{
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
    int lib_loaded = 0, rc = -1;

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

    collect_shipped_files(dig, tab, &wanted, &report->skipped);
    if (wanted.count == 0) {
        err_set(err, errsz, "the digest lists no installable files for this tab");
        goto done;
    }

    files = xmalloc(wanted.count * sizeof(*files));
    for (i = 0; i < wanted.count; i++) {
        files[count].name = str_dup(wanted.items[i]);
        files[count].target = path_join(game_dir, wanted.items[i]);
        files[count].backup = str_fmt("%s%s", files[count].target, INSTALL_BACKUP_SUFFIX);
        files[count].source = NULL;
        files[count].data = NULL;
        files[count].len = 0;
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
    if (no_backup.count > 0) {
        char *names = list_to_text(&no_backup);
        err_set(err, errsz, "%u original(s) are missing their '%s' backup: %s. "
                            "Restoring them would leave the game without those files",
                (unsigned)no_backup.count, INSTALL_BACKUP_SUFFIX, names);
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

    /* Keep the tab's copies in memory, so a failure part-way can be undone. */
    for (i = 0; i < count; i++) {
        files[i].data = plat_read_file(files[i].target, &files[i].len);
        if (!files[i].data) {
            err_set(err, errsz, "cannot read '%s'", files[i].target);
            goto done;
        }
    }

    /*
     * --- Apply, undoing the install in reverse order: the library first,
     * then the level files ---
     */
    if (lib_write_uri(&img, LIB_OFFICIAL_URI, err, errsz) != 0)
        goto done;

    for (i = 0; i < count; i++) {
        if (plat_replace_file(files[i].backup, files[i].target) != 0) {
            err_set(err, errsz, "cannot restore '%s' from '%s'; is the game running?",
                    files[i].target, files[i].backup);
            /* Put back what was already restored: move the original aside
             * again, rewrite the tab's copy, and re-point the library. */
            while (restored-- > 0) {
                plat_replace_file(files[restored].target, files[restored].backup);
                plat_write_file(files[restored].target, files[restored].data,
                                files[restored].len);
            }
            lib_write_uri(&img, patched_uri, cfg_err, sizeof cfg_err);
            goto done;
        }
        restored++;
    }

    report->restored_count = restored;
    report->game_levels_dir = game_dir;
    collect_leftover_backups(report->game_levels_dir, &wanted, &report->leftovers);
    game_dir = NULL;                 /* ownership moves to the report */
    snprintf(report->server_uri, sizeof report->server_uri, "%s", LIB_OFFICIAL_URI);

    /* --- Record it. install_date is left alone: it is still true. --- */
    config_set_uninstalled(state, tab->id, tab->code);
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
    if (state)
        config_free(state);
    install_files_free(files, count);
    str_list_free(&wanted);
    str_list_free(&missing);
    str_list_free(&no_backup);
    str_list_free(&known);
    free(game_dir);
    return rc;
}

void uninstall_report_free(uninstall_report *report)
{
    free(report->game_levels_dir);
    str_list_free(&report->skipped);
    str_list_free(&report->leftovers);
    report->game_levels_dir = NULL;
}
