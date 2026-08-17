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
 *     "remove_date": null
 *   }
 */
#ifndef TABBER_CONFIG_H
#define TABBER_CONFIG_H

#include <stddef.h>

#include "json.h"

#define CONFIG_FILENAME      "config.json"
#define CONFIG_TMP_SUFFIX    ".tmp"

/* Keys of the state file. */
#define CJK_TABS             "tabs"
#define CJK_ID               "id"
#define CJK_CODE             "code"
#define CJK_DOWNLOADED       "downloaded"
#define CJK_INSTALLED        "installed"
#define CJK_DOWNLOAD_DATE    "download_date"
#define CJK_INSTALL_DATE     "install_date"
#define CJK_UNINSTALL_DATE   "uninstall_date"
#define CJK_REMOVE_DATE      "remove_date"

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

/*
 * The entry for `code`, created with all fields at their defaults (not
 * downloaded, not installed, every date null) if it is not there yet.
 */
json_value *config_tab_entry(config *cfg, int id, const char *code);

/* Records that a tab has just been downloaded: sets the flag and the date. */
void config_set_downloaded(config *cfg, int id, const char *code);

#endif /* TABBER_CONFIG_H */
