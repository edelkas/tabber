#include <stdio.h>
#include <stdlib.h>

#include "json.h"
#include "platform.h"
#include "save.h"
#include "usage.h"
#include "util.h"

/* When `dir`/`name` was last written. Returns 0 and fills `out` on success. */
static int mtime_in(const char *dir, const char *name, long long *out)
{
    char *path;
    int rc;

    if (!dir)
        return -1;
    path = path_join(dir, name);
    rc = plat_file_mtime(path, out);
    free(path);
    return rc;
}

/* A date field of the tab's state entry, as a timestamp; 0 when it has none. */
static long long recorded_date(config *cfg, const char *code, const char *key)
{
    json_value *entry = cfg ? config_find_tab(cfg, code) : NULL;

    return entry ? time_from_iso8601(json_get_string(entry, key, NULL)) : 0;
}

static void answer(tab_usage *out, long long when, usage_source source)
{
    out->when = when;
    out->source = source;
}

void usage_last_played(config *cfg, const npp_paths *paths, const char *code,
                       int installed, tab_usage *out)
{
    const char *dir = paths ? paths->personal_dir : NULL;
    char archive[64];
    char *lower;
    long long when = 0;

    answer(out, 0, USAGE_NEVER);
    if (!code)
        return;

    if (installed) {
        /* The live save is this tab's for as long as it is in place, so the
         * last time the game wrote it is the last time the tab was played.
         * The gzipped form is preferred, exactly as the game prefers it. */
        if (mtime_in(dir, SAVE_GZ_NAME, &when) == 0 ||
            mtime_in(dir, SAVE_NAME, &when) == 0) {
            answer(out, when, USAGE_LIVE_SAVE);
            return;
        }
        /* No savefile at all: the game has not been run since the install, so
         * the install itself is the most recent thing that happened to it. */
        when = recorded_date(cfg, code, CJK_INSTALL_DATE);
        if (when)
            answer(out, when, USAGE_RECORDED);
        return;
    }

    /* Uninstalled: when its save stopped being the live one. */
    when = recorded_date(cfg, code, CJK_UNINSTALL_DATE);
    if (when) {
        answer(out, when, USAGE_RECORDED);
        return;
    }

    /* Nothing written down, which is what a tab installed by one of the older
     * installers looks like. Their archives are named the same as ours, and
     * one was written the moment the tab came out. */
    lower = str_dup_lower(code);
    snprintf(archive, sizeof archive, SAVE_BACKUP_FMT, lower ? lower : code);
    free(lower);
    if (mtime_in(dir, archive, &when) == 0)
        answer(out, when, USAGE_ARCHIVED_SAVE);
}
