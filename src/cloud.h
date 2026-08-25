/*
 * cloud.h - The savefile's other copy, the one Steam keeps.
 *
 * N++ syncs its savefile through Steam Cloud, whose local copy lives in
 *
 *   <Steam folder>/userdata/<Steam32ID>/230270/remote/
 *
 * and which takes precedence over the one in the personal folder: swap the
 * local save without touching this, and Steam puts the old one back before the
 * game ever reads it. Every Steam account on the machine has its own folder
 * there and we cannot know which one is playing, so every account that has N++
 * data is treated the same way.
 *
 * What happens to a cloud save is the caller's choice (see cloud_mode), with
 * two rules that are not:
 *
 *   - An uncompressed cloud save is always deleted rather than replaced. Steam
 *     Cloud was switched off in 2023 over corruption problems and only came
 *     back with TEN++, which gzips; anything uncompressed up there predates
 *     that, is in a different and undocumented format that fitted the 3 MB
 *     quota, and is of no use to any current build.
 *   - A replacement is always gzipped, whatever form the local save is in. The
 *     uncompressed savefile is 70 MB and would not fit the quota at all.
 *
 * Nothing is archived here. The savefile these copies mirror is archived by
 * save.c, which is where anything worth keeping already ends up.
 */
#ifndef TABBER_CLOUD_H
#define TABBER_CLOUD_H

#include <stddef.h>

#include "paths.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Steam's own layout, under its base folder. */
#define CLOUD_USERDATA_DIR  "userdata"
#define CLOUD_REMOTE_DIR    "remote"

/* What to do with a gzipped cloud save that is found. */
typedef enum {
    CLOUD_REPLACE,   /* overwrite it with the save going in, gzipped */
    CLOUD_REMOVE,    /* delete it, and let the game upload a new one */
    CLOUD_KEEP       /* leave Steam's folder alone; only report it    */
} cloud_mode;

/* The mode named by `text` ("replace", "remove", "keep"), or -1. */
int cloud_mode_parse(const char *text, cloud_mode *out);
const char *cloud_mode_name(cloud_mode mode);

/* What was found for one Steam account, and what became of it. */
typedef struct {
    char *id;                  /* the Steam32ID, as the folder names it */
    char *dir;                 /* its remote folder                     */
    int had_raw;               /* an uncompressed cloud save was there  */
    int had_gz;                /* a gzipped one was there               */
    int removed_raw;
    int removed_gz;
    int replaced;
    size_t written;            /* bytes written, when replaced          */
    char detail[TB_ERR_LEN];   /* what went wrong, if anything did      */
} cloud_user;

typedef struct {
    cloud_user *users;
    size_t count;              /* accounts that have N++ cloud data     */
    size_t touched;            /* ...of which had a save to act on      */
    size_t failed;             /* ...of which something went wrong for  */
    int searched;              /* Steam's folder was found and looked at */
} cloud_report;

/*
 * Applies `mode` to every account that has N++ cloud data. `save`/`save_len`
 * are the savefile that just went into the personal folder, in whatever form
 * it took; it is gzipped here if a replacement calls for it. Returns 0 when
 * the search itself could be carried out — per-account trouble is reported in
 * `report`, not returned — and -1 with a reason in `err` when it could not.
 */
int cloud_apply(const npp_paths *paths, cloud_mode mode,
                const unsigned char *save, size_t save_len,
                cloud_report *report, char *err, size_t errsz);

void cloud_report_free(cloud_report *report);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_CLOUD_H */
