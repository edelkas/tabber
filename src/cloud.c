#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloud.h"
#include "gzip.h"
#include "platform.h"
#include "save.h"
#include "util.h"

/* ---- Modes ------------------------------------------------------------- */

int cloud_mode_parse(const char *text, cloud_mode *out)
{
    if (!text)
        return -1;
    if (str_ieq(text, "replace")) { *out = CLOUD_REPLACE; return 0; }
    if (str_ieq(text, "remove"))  { *out = CLOUD_REMOVE;  return 0; }
    if (str_ieq(text, "keep"))    { *out = CLOUD_KEEP;    return 0; }
    return -1;
}

const char *cloud_mode_name(cloud_mode mode)
{
    switch (mode) {
        case CLOUD_REPLACE: return "replace";
        case CLOUD_REMOVE:  return "remove";
        case CLOUD_KEEP:    return "keep";
        default:            return "?";
    }
}

/* ---- Finding the accounts ---------------------------------------------- */

/* Steam32IDs are decimal, and nothing else in userdata/ is. */
static int is_account_id(const char *name)
{
    size_t i;

    if (!name || !*name)
        return 0;
    for (i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9')
            return 0;
    }
    return 1;
}

/* <steam>/userdata/<id>/230270/remote, if that is really a folder. */
static char *remote_dir_for(const char *userdata, const char *id)
{
    char *user = path_join(userdata, id);
    char *app = path_join(user, NPP_STEAM_APPID);
    char *remote = path_join(app, CLOUD_REMOTE_DIR);

    free(user);
    free(app);
    if (plat_is_dir(remote))
        return remote;
    free(remote);
    return NULL;
}

/* ---- Acting on one account --------------------------------------------- */

/*
 * The save to upload, gzipped. Built once and reused, since every account gets
 * the same one, and checked by unpacking it again: a cloud save the game
 * cannot read would be worse than none at all.
 */
static const unsigned char *cloud_payload(const unsigned char *save, size_t save_len,
                                          unsigned char **cache, size_t *cache_len,
                                          size_t *out_len, char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];
    unsigned char *packed, *back;
    size_t packed_len = 0, back_len = 0;

    if (gz_is_gzip(save, save_len)) {      /* already the form the cloud needs */
        *out_len = save_len;
        return save;
    }
    if (*cache) {
        *out_len = *cache_len;
        return *cache;
    }

    packed = gz_compress(save, save_len, SAVE_NAME, &packed_len);
    back = gz_extract(packed, packed_len, &back_len, sub, sizeof sub);
    if (!back || back_len != save_len || memcmp(back, save, save_len) != 0) {
        err_set(err, errsz, "the savefile did not survive being compressed for the "
                            "cloud (%s)", back ? "it came back different" : sub);
        free(back);
        free(packed);
        return NULL;
    }
    free(back);

    *cache = packed;
    *cache_len = packed_len;
    *out_len = packed_len;
    return packed;
}

/* Writes a cloud save through a temporary file, so Steam never sees half of one. */
static int write_cloud_file(cloud_user *user, const char *name,
                            const unsigned char *data, size_t len)
{
    char *path = path_join(user->dir, name);
    char *tmp = str_fmt("%s%s", path, SAVE_TMP_SUFFIX);
    int rc = -1;

    if (plat_write_file(tmp, data, len) != 0) {
        snprintf(user->detail, sizeof user->detail, "'%s' could not be written", tmp);
    } else if (plat_replace_file(tmp, path) != 0) {
        snprintf(user->detail, sizeof user->detail, "'%s' could not be put in place", path);
        plat_remove_file(tmp);
    } else {
        rc = 0;
    }

    free(tmp);
    free(path);
    return rc;
}

/* Deletes one file, noting what happened on the account's line of the report. */
static int remove_cloud_file(cloud_user *user, const char *name)
{
    char *path = path_join(user->dir, name);
    int ok = plat_remove_file(path) == 0;

    if (!ok)
        snprintf(user->detail, sizeof user->detail, "'%s' could not be removed", path);
    free(path);
    return ok ? 0 : -1;
}

/* ---- The whole sweep --------------------------------------------------- */

int cloud_apply(const npp_paths *paths, cloud_mode mode,
                const unsigned char *save, size_t save_len,
                cloud_report *report, char *err, size_t errsz)
{
    char *userdata = NULL;
    str_list entries = {0};
    unsigned char *packed = NULL;   /* the gzipped payload, made on demand */
    size_t packed_len = 0;
    size_t i;
    int rc = -1;

    memset(report, 0, sizeof(*report));

    /*
     * No Steam folder means no cloud copies to worry about: that is the case
     * for a game found through TABBER_GAME_DIR, and not a failure.
     */
    if (!paths->steam_dir || !*paths->steam_dir)
        return 0;

    userdata = path_join(paths->steam_dir, CLOUD_USERDATA_DIR);
    if (!plat_is_dir(userdata)) {
        free(userdata);
        return 0;               /* Steam is there, but no account data is */
    }
    report->searched = 1;

    if (plat_list_dir(userdata, &entries) != 0) {
        err_set(err, errsz, "cannot read '%s'", userdata);
        free(userdata);
        return -1;
    }
    str_list_sort(&entries);    /* accounts in a stable order, for the log */

    report->users = entries.count ? xmalloc(entries.count * sizeof(*report->users)) : NULL;
    for (i = 0; i < entries.count; i++) {
        cloud_user *user;
        char *remote, *path;

        if (!is_account_id(entries.items[i]))
            continue;
        remote = remote_dir_for(userdata, entries.items[i]);
        if (!remote)
            continue;           /* this account does not have N++ */

        user = &report->users[report->count++];
        memset(user, 0, sizeof(*user));
        user->id = str_dup(entries.items[i]);
        user->dir = remote;

        path = path_join(remote, SAVE_NAME);
        user->had_raw = plat_is_file(path);
        free(path);
        path = path_join(remote, SAVE_GZ_NAME);
        user->had_gz = plat_is_file(path);
        free(path);

        if (!user->had_raw && !user->had_gz)
            continue;           /* nothing up there: nothing to put right */
        if (mode == CLOUD_KEEP) {
            report->touched++;  /* found, and deliberately left alone */
            continue;
        }

        /* The uncompressed one goes whatever the mode: it can only be a
         * leftover from before Steam Cloud was switched off in 2023. */
        if (user->had_raw) {
            if (remove_cloud_file(user, SAVE_NAME) == 0)
                user->removed_raw = 1;
            else
                report->failed++;
        }

        if (user->had_gz && mode == CLOUD_REMOVE) {
            if (remove_cloud_file(user, SAVE_GZ_NAME) == 0)
                user->removed_gz = 1;
            else
                report->failed++;
        } else if (user->had_gz) {
            const unsigned char *body;
            size_t body_len = 0;

            body = cloud_payload(save, save_len, &packed, &packed_len, &body_len,
                                 err, errsz);
            if (!body)
                goto done;      /* the payload is everyone's: stop here */

            if (write_cloud_file(user, SAVE_GZ_NAME, body, body_len) == 0) {
                user->replaced = 1;
                user->written = body_len;
            } else {
                report->failed++;
            }
        }

        report->touched++;
    }

    rc = 0;

done:
    free(packed);
    str_list_free(&entries);
    free(userdata);
    return rc;
}

void cloud_report_free(cloud_report *report)
{
    size_t i;

    for (i = 0; i < report->count; i++) {
        free(report->users[i].id);
        free(report->users[i].dir);
    }
    free(report->users);
    memset(report, 0, sizeof(*report));
}
