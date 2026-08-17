#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "install.h"
#include "json.h"
#include "platform.h"
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
    install_file *files = NULL;
    size_t count = 0, i, done = 0;
    char *game_dir = NULL, *tab_dir = NULL, *tab_root = NULL;
    char cfg_err[TB_ERR_LEN];
    config *state;
    int rc = -1;

    memset(report, 0, sizeof(*report));

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
        char *source = path_join(tab_dir, name);

        if (!plat_is_file(source)) {       /* subfolders are not ours to install */
            free(source);
            continue;
        }
        if (!str_list_contains(&supported, name)) {
            /* Not a file the game reads: leave it behind and say so. */
            str_list_push(&report->skipped, str_dup(name));
            free(source);
            continue;
        }

        files[count].name = str_dup(name);
        files[count].source = source;
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

    report->installed_count = done;
    report->game_levels_dir = game_dir;
    report->tab_levels_dir = tab_dir;
    game_dir = tab_dir = NULL;               /* ownership moves to the report */

    /* --- Record it --- */
    state = config_load(cfg_err, sizeof cfg_err);
    if (!state) {
        err_set(report->warning, sizeof report->warning,
                "the install was not recorded: %s", cfg_err);
    } else {
        config_set_installed(state, tab->id, tab->code);
        if (config_save(state, cfg_err, sizeof cfg_err) != 0)
            err_set(report->warning, sizeof report->warning,
                    "the install was not recorded: %s", cfg_err);
        else
            snprintf(report->state_path, sizeof report->state_path, "%s", state->path);
        config_free(state);
    }
    rc = 0;

done:
    install_files_free(files, count);
    str_list_free(&supported);
    str_list_free(&entries);
    str_list_free(&missing);
    str_list_free(&conflicts);
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
