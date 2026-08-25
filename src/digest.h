/*
 * digest.h - The custom tab catalogue.
 *
 * A JSON digest published alongside the mappacks lists every supported custom
 * tab. It is cached in the tool's folder so the tool still works offline,
 * and refreshed from the network once per session (see digest_ensure_fresh).
 */
#ifndef TABBER_DIGEST_H
#define TABBER_DIGEST_H

#include <stddef.h>

#include "json.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Source of the digest. The canonical page is
 * https://github.com/edelkas/inne/blob/master/db/mappacks/digest.json,
 * which serves HTML; this is the raw file behind it. Overridable at build time
 * to point the tool at a staging server.
 */
#ifndef DIGEST_URL
#define DIGEST_URL          "https://raw.githubusercontent.com/edelkas/inne/master/db/mappacks/digest.json"
#endif

/* On-disk cache, kept in the tool's folder. */
#define DIGEST_FILENAME     "digest.json"
#define DIGEST_TMP_SUFFIX   ".tmp"     /* staging name for atomic replacement */

/* JSON keys of the digest. */
#define DJK_CONFIG          "config"
#define DJK_LEVELS_DIR      "levels_dir"
#define DJK_PALETTES_DIR    "palettes_dir"
#define DJK_LEVEL_FILES     "level_files"       /* files the game reads */
#define DJK_CHALLENGE_FILES "challenge_files"
#define DJK_TABS            "tabs"
#define DJK_ATTRIBUTES      "attributes"
#define DJK_SIGNATURE       "signature"
#define DJK_ID              "id"
#define DJK_NAME            "name"
#define DJK_CODE            "code"
#define DJK_AUTHORS         "authors"
#define DJK_DATE            "date"
#define DJK_VERSION         "version"
#define DJK_ENABLED         "enabled"
#define DJK_MD5             "md5"

/* Length of the "YYYY-MM-DD" prefix of an ISO 8601 timestamp. */
#define DIGEST_DATE_LEN     10

/* Used when the digest omits the folder names, as the game names them today. */
#define DIGEST_DEFAULT_LEVELS_DIR   "Levels"
#define DIGEST_DEFAULT_PALETTES_DIR "Palettes"

/* One custom tab. Strings are borrowed from the parsed document. */
typedef struct {
    int id;                    /* index in the catalogue, 0-based    */
    const char *code;          /* 3-letter code, lowercase as stored */
    const char *name;
    const char *authors;
    const char *date;          /* ISO 8601 release timestamp         */
    int version;
    int enabled;
    const json_value *node;    /* full entry, for the download/disk/... keys */
} npp_tab;

/*
 * Column headers of a tab listing, and buffers wide enough for the two cells
 * that are not shown verbatim. Both front-ends list the same four fields under
 * the same names, so the names live here rather than in either of them.
 */
#define COL_CODE            "CODE"
#define COL_NAME            "NAME"
#define COL_AUTHORS         "AUTHOR(S)"
#define COL_DATE            "RELEASED"
#define DIGEST_CODE_BUF     16
#define DIGEST_DATE_BUF     (DIGEST_DATE_LEN + 1)

/* A parsed digest. Owns the JSON document its strings point into. */
typedef struct {
    json_value *root;
    npp_tab *tabs;
    size_t tab_count;
    const char *signature_date;
    const char *signature_md5;
    char *path;                /* file it was loaded from */
} digest;

/*
 * Downloads the digest and replaces the on-disk cache. Unless `force` is set,
 * the download happens only once per session; later calls are no-ops that
 * succeed. Returns 0 on success, -1 with a reason in `err`.
 */
int digest_ensure_fresh(int force, char *err, size_t errsz);

/* Parses the cached digest. Returns NULL with a reason in `err`. */
digest *digest_load(char *err, size_t errsz);

void digest_free(digest *dig);

/*
 * The two fields a listing does not show as stored: the code goes up, as the
 * game shows it, and the date keeps its "YYYY-MM-DD" part. Both truncate to
 * fit and always terminate; a NULL input gives an empty string.
 */
void digest_code_upper(char *out, size_t outsz, const char *code);
void digest_date_short(char *out, size_t outsz, const char *iso);

/* Path of the on-disk cache. Caller frees. */
char *digest_cache_path(void);

/* Looks up a tab by its code, case-insensitively. NULL when absent. */
const npp_tab *digest_find(const digest *dig, const char *code);

/*
 * Global settings block, and the folder names inside a tab archive. The game
 * may rename these one day, which is why the digest carries them.
 */
const json_value *digest_config(const digest *dig);
const char *digest_levels_dir(const digest *dig);
const char *digest_palettes_dir(const digest *dig);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_DIGEST_H */
