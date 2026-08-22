/*
 * config.h - The tool's own configuration and state, kept in config.json next
 * to the executable.
 *
 * The file is read into a JSON tree, modified in place and written back, so
 * keys this version does not know about (hand-edited settings, fields added by
 * a later version) survive untouched. Writes are staged and swapped in, like
 * the digest cache.
 *
 * The "tabs" array tracks what has happened to each custom tab:
 *
 *   {
 *     "id": 21,
 *     "code": "lit",
 *     "downloaded": true,
 *     "installed": false,
 *     "download_date": "2026-08-17T18:42:03Z",
 *     "install_date": null,
 *     "uninstall_date": null,
 *     "remove_date": null,
 *     "palettes": ["nova cosmic", "nova orbit"]
 *   }
 *
 * "palettes" holds the folders an install actually created in the game's
 * palettes folder, so uninstalling deletes those and no others: a palette that
 * was skipped because the user already had one of that name is not in the list
 * and is therefore never mistaken for the tab's.
 */
#ifndef TABBER_CONFIG_H
#define TABBER_CONFIG_H

#include <stddef.h>

#include "json.h"
#include "util.h"

#define CONFIG_FILENAME      "config.json"
#define CONFIG_TMP_SUFFIX    ".tmp"

/* Keys of the state file. */
#define CJK_TABS             "tabs"
#define CJK_STATE            "state"     /* what the game files look like */
#define CJK_LIBRARY          "library"   /* did the library check pass?   */
#define CJK_ID               "id"
#define CJK_CODE             "code"
#define CJK_DOWNLOADED       "downloaded"
#define CJK_INSTALLED        "installed"
#define CJK_DOWNLOAD_DATE    "download_date"
#define CJK_INSTALL_DATE     "install_date"
#define CJK_UNINSTALL_DATE   "uninstall_date"
#define CJK_REMOVE_DATE      "remove_date"
#define CJK_PALETTES         "palettes"   /* folders an install created    */

typedef struct {
    json_value *root;   /* whole document, "tabs" included */
    char *path;
} config;

/* Path of the state file. Caller frees. */
char *config_path(void);

/*
 * Loads the state file, or starts an empty one when it does not exist yet.
 * Returns NULL with a reason in `err` if the file exists but cannot be used;
 * in that case nothing is overwritten, so a hand-edited file is never lost.
 */
config *config_load(char *err, size_t errsz);

/* Writes the state back out. Returns 0 on success. */
int config_save(config *cfg, char *err, size_t errsz);

void config_free(config *cfg);

/* The entry for `code`, or NULL when the tab has no history yet. */
json_value *config_find_tab(config *cfg, const char *code);

/*
 * Records whether the game's library still matches what we believe is
 * installed, under "state" -> "library". Refreshed on every check.
 */
void config_set_state_library(config *cfg, int ok);

/*
 * The entry for `code`, created with all fields at their defaults (not
 * downloaded, not installed, every date null) if it is not there yet.
 */
json_value *config_tab_entry(config *cfg, int id, const char *code);

/* Records that a tab has just been downloaded: sets the flag and the date. */
void config_set_downloaded(config *cfg, int id, const char *code);

/* Records that a tab has just been installed into the game. */
void config_set_installed(config *cfg, int id, const char *code);

/*
 * Records that a tab has just been uninstalled: clears "installed" and stamps
 * "uninstall_date". "install_date" is deliberately kept, so when the tab was
 * last installed is not lost.
 */
void config_set_uninstalled(config *cfg, int id, const char *code);

/*
 * Records that a tab's files have been removed: clears "downloaded" and stamps
 * "remove_date". The entry itself is kept, so a later re-download still has its
 * history. `id` may be -1 when unknown, in which case an entry is only updated,
 * never created. Returns 1 if anything was recorded.
 */
int config_set_removed(config *cfg, int id, const char *code);

/*
 * Records the palette folders an install created, replacing whatever was
 * listed before. An empty list clears the record, which is what uninstalling
 * does once they are gone.
 */
void config_set_palettes(config *cfg, int id, const char *code, const str_list *names);

/*
 * Reads that record back. Returns 1 when the tab has one at all, which an
 * install that put no palette in still does: an empty record means "none went
 * in", and is not the same as never having recorded any.
 */
int config_get_palettes(config *cfg, const char *code, str_list *out);

#endif /* TABBER_CONFIG_H */
