#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gzip.h"
#include "platform.h"
#include "save.h"
#include "util.h"
#include "zip.h"

/* ---- Helpers ----------------------------------------------------------- */

/* Confirms the personal folder takes new files before anything is written. */
static int dir_is_writable(const char *dir)
{
    char *probe = path_join(dir, SAVE_PROBE_FILE);
    int ok = plat_write_file(probe, "", 0) == 0;

    if (ok)
        plat_remove_file(probe);
    free(probe);
    return ok;
}

/*
 * Reads the savefile out of one of the archives. The entry is looked up by
 * prefix, as the old installers did, and its CRC-32 is checked on the way out,
 * so a damaged archive is caught here rather than at the game's next launch.
 */
static int archive_extract_save(const char *path, unsigned char **out, size_t *len_out,
                                char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];
    char *data;
    size_t len;
    zip_archive zip;
    const zip_entry *entry;
    unsigned char *save;

    *out = NULL;
    *len_out = 0;

    data = plat_read_file(path, &len);
    if (!data) {
        err_set(err, errsz, "cannot read '%s'", path);
        return -1;
    }
    if (zip_open(&zip, data, len, sub, sizeof sub) != 0) {
        err_set(err, errsz, "'%s': %s", path, sub);
        free(data);
        return -1;
    }

    entry = zip_find_prefix(&zip, SAVE_ENTRY_PREFIX);
    if (!entry) {
        err_set(err, errsz, "'%s' holds no '%s' file", path, SAVE_ENTRY_PREFIX "*");
        zip_close(&zip);
        free(data);
        return -1;
    }

    save = zip_read(&zip, entry, sub, sizeof sub);
    if (!save)
        err_set(err, errsz, "'%s': %s", path, sub);
    else
        *len_out = entry->uncomp_size;

    *out = save;
    zip_close(&zip);
    free(data);
    return save ? 0 : -1;
}

/*
 * Whether a gzip stream really unpacks to the bytes it was made from. The
 * compressor is ours, so this is the only thing standing between a mistake in
 * it and a savefile the game cannot read.
 */
static int unpacks_to(const unsigned char *packed, size_t packed_len,
                      const unsigned char *original, size_t len)
{
    char err[TB_ERR_LEN];
    size_t back_len = 0;
    unsigned char *back = gz_extract(packed, packed_len, &back_len, err, sizeof err);
    int same = back && back_len == len && memcmp(back, original, len) == 0;

    free(back);
    return same ? 0 : -1;
}

/* Writes `data` to `path` by way of a temporary file, then reads it back. */
static int write_verified(const char *path, const unsigned char *data, size_t len,
                          char *err, size_t errsz)
{
    char *tmp = str_fmt("%s%s", path, SAVE_TMP_SUFFIX);
    char *check;
    size_t check_len = 0;
    int rc = -1;

    if (plat_write_file(tmp, data, len) != 0) {
        err_set(err, errsz, "cannot write '%s'", tmp);
        goto done;
    }

    /* Read it back rather than trusting the write: this is the one file the
     * user cannot get back if it goes wrong. */
    check = plat_read_file(tmp, &check_len);
    if (!check || check_len != len || crc32_bytes(check, check_len) != crc32_bytes(data, len)) {
        err_set(err, errsz, "'%s' did not read back as it was written", tmp);
        free(check);
        plat_remove_file(tmp);
        goto done;
    }
    free(check);

    if (plat_replace_file(tmp, path) != 0) {
        err_set(err, errsz, "cannot move '%s' into place as '%s'; is the game running?",
                tmp, path);
        plat_remove_file(tmp);
        goto done;
    }
    rc = 0;

done:
    free(tmp);
    return rc;
}

/* ---- The save tabber ships --------------------------------------------- */

/* <dir>/res/nprofile.zip if it is there, else <dir>/nprofile.zip. */
static char *fresh_under(char *dir)
{
    char *path, *file;

    if (!dir)
        return NULL;

    path = path_join(dir, SAVE_RES_DIR);
    file = path_join(path, SAVE_FRESH_ZIP);
    free(path);
    if (plat_is_file(file)) {
        free(dir);
        return file;
    }
    free(file);

    file = path_join(dir, SAVE_FRESH_ZIP);
    free(dir);
    if (plat_is_file(file))
        return file;
    free(file);
    return NULL;
}

char *save_fresh_path(void)
{
    char *override = plat_getenv(TABBER_ENV_FRESH_SAVE);
    char *path;

    if (override) {
        if (plat_is_file(override))
            return override;
        free(override);
    }

    /* It ships with the program, so the executable's own folder is where it
     * belongs; the tool's root is tried first for portable layouts, and the
     * two are the same folder unless TABBER_HOME says otherwise. */
    path = fresh_under(plat_app_root());
    return path ? path : fresh_under(plat_exe_dir());
}

/* ---- Planning ---------------------------------------------------------- */

int save_plan_build(const npp_paths *paths, const char *code, int installing,
                    unsigned flags, save_plan *plan, char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];
    char *gz_path = NULL, *raw_path = NULL, *tab_archive = NULL, *lower = NULL;
    unsigned char *live = NULL, *data = NULL;
    size_t data_len = 0;
    int rc = -1;

    memset(plan, 0, sizeof(*plan));

    if (!paths->personal_dir || !plat_is_dir(paths->personal_dir)) {
        err_set(err, errsz, "N++'s personal folder was not found, so the savefile cannot "
                            "be swapped; has the game been run at least once?");
        return -1;
    }
    plan->dir = str_dup(paths->personal_dir);
    if (!dir_is_writable(plan->dir)) {
        err_set(err, errsz, "cannot write to '%s'; check the folder's permissions and "
                            "that the game is not running", plan->dir);
        goto done;
    }

    gz_path = path_join(plan->dir, SAVE_GZ_NAME);
    raw_path = path_join(plan->dir, SAVE_NAME);

    /* Which save is live: the gzipped one wins, as the game reads it first. */
    if (plat_is_file(gz_path)) {
        plan->form = SAVE_GZIPPED;
        plan->live_path = str_dup(gz_path);
        plan->entry_name = str_dup(SAVE_GZ_NAME);
    } else if (plat_is_file(raw_path)) {
        plan->form = SAVE_RAW;
        plan->live_path = str_dup(raw_path);
        plan->entry_name = str_dup(SAVE_NAME);
    } else {
        plan->form = SAVE_ABSENT;      /* nothing to archive, and no proof of
                                          what the game can read: play it safe */
    }

    /* Where the old save goes and where the new one comes from. Installing
     * files the vanilla save away and brings the tab's back; uninstalling is
     * the same trade the other way round. */
    lower = str_dup_lower(code);
    tab_archive = str_fmt(SAVE_BACKUP_FMT, lower);
    if (installing) {
        plan->backup_path = path_join(plan->dir, SAVE_BACKUP_ORIGINAL);
        plan->source_path = path_join(plan->dir, tab_archive);
    } else {
        plan->backup_path = path_join(plan->dir, tab_archive);
        plan->source_path = path_join(plan->dir, SAVE_BACKUP_ORIGINAL);
    }

    /* No archive for this direction yet: this tab has never been played, or
     * the vanilla save was never filed away. Use the one tabber ships. */
    if (!plat_is_file(plan->source_path)) {
        char *fresh = save_fresh_path();

        if (!fresh) {
            err_set(err, errsz, "'%s' is not there and tabber's own '%s%c%s' could not be "
                                "found either, so there is no savefile to put in place",
                    plan->source_path, SAVE_RES_DIR, PATH_SEP, SAVE_FRESH_ZIP);
            goto done;
        }
        free(plan->source_path);
        plan->source_path = fresh;
        plan->used_fresh = 1;
    }

    if (archive_extract_save(plan->source_path, &data, &data_len, err, errsz) != 0)
        goto done;
    if (data_len == 0) {
        err_set(err, errsz, "the savefile in '%s' is empty", plan->source_path);
        goto done;
    }

    /*
     * Which form to write it in. A gzipped save is only kept gzipped when the
     * game has shown it understands that (by having one on disk); otherwise it
     * is unwrapped. A save that is not compressed is normally left that way,
     * since every build reads the uncompressed file and TEN++ and later
     * re-compress it themselves on their next save — unless the caller asks
     * for it to be compressed anyway, which is only allowed on the same
     * evidence that the game reads gzip at all.
     */
    if (gz_is_gzip(data, data_len) && plan->form == SAVE_GZIPPED) {
        plan->save = data;
        plan->save_len = data_len;
        data = NULL;
        plan->save_path = str_dup(gz_path);
        plan->other_path = str_dup(raw_path);
    } else if (!gz_is_gzip(data, data_len) && plan->form == SAVE_GZIPPED &&
               (flags & SAVE_FORCE_COMPRESS)) {
        /* Asked for, and allowed: the game reads gzip, so hand it a gzipped
         * save instead of one it would have to fall back to. */
        plan->save = gz_compress(data, data_len, SAVE_NAME, &plan->save_len);
        plan->compressed = 1;
        plan->save_path = str_dup(gz_path);
        plan->other_path = str_dup(raw_path);

        /* Read our own work back before trusting it to the game: a savefile
         * the game cannot open would only show up at its next launch, by
         * which time the copy this came from may be gone. */
        if (unpacks_to(plan->save, plan->save_len, data, data_len) != 0) {
            err_set(err, errsz, "the savefile did not survive being compressed; "
                                "nothing was changed. Install without "
                                "--force-compress to put it in place as it is");
            goto done;
        }
    } else if (gz_is_gzip(data, data_len)) {
        plan->save = gz_extract(data, data_len, &plan->save_len, sub, sizeof sub);
        if (!plan->save) {
            err_set(err, errsz, "'%s': %s", plan->source_path, sub);
            goto done;
        }
        plan->save_path = str_dup(raw_path);
        plan->other_path = str_dup(gz_path);
    } else {
        plan->save = data;
        plan->save_len = data_len;
        data = NULL;
        plan->save_path = str_dup(raw_path);
        plan->other_path = str_dup(gz_path);
    }

    /* Finally, the archive of what is there now, built but not yet written. */
    if (plan->form != SAVE_ABSENT) {
        live = (unsigned char *)plat_read_file(plan->live_path, &plan->live_len);
        if (!live) {
            err_set(err, errsz, "cannot read the savefile '%s'", plan->live_path);
            goto done;
        }
        if (plan->live_len == 0) {
            err_set(err, errsz, "the savefile '%s' is empty; refusing to touch it until "
                                "you have looked at it", plan->live_path);
            goto done;
        }
        plan->backup = zip_create_stored(plan->entry_name, live, plan->live_len,
                                         &plan->backup_len);
    }

    rc = 0;

done:
    free(live);
    free(data);
    free(lower);
    free(tab_archive);
    free(gz_path);
    free(raw_path);
    if (rc != 0)
        save_plan_free(plan);
    return rc;
}

/* ---- Applying ---------------------------------------------------------- */

int save_plan_apply(save_plan *plan, save_report *report, char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];

    memset(report, 0, sizeof(*report));

    /* 1. File the live save away, and prove the archive is readable before
     *    the save it holds is overwritten. */
    if (plan->backup) {
        unsigned char *check = NULL;
        size_t check_len = 0;
        char *tmp = str_fmt("%s%s", plan->backup_path, SAVE_TMP_SUFFIX);
        int ok;

        if (plat_write_file(tmp, plan->backup, plan->backup_len) != 0) {
            err_set(err, errsz, "cannot write the savefile backup '%s'", tmp);
            free(tmp);
            return -1;
        }
        ok = archive_extract_save(tmp, &check, &check_len, sub, sizeof sub) == 0 &&
             check_len == plan->live_len;
        free(check);
        if (!ok) {
            err_set(err, errsz, "the savefile backup did not verify (%s); the savefile "
                                "was left alone",
                    check_len != plan->live_len ? "it holds the wrong number of bytes" : sub);
            plat_remove_file(tmp);
            free(tmp);
            return -1;
        }
        if (plat_replace_file(tmp, plan->backup_path) != 0) {
            err_set(err, errsz, "cannot move the savefile backup into place as '%s'",
                    plan->backup_path);
            plat_remove_file(tmp);
            free(tmp);
            return -1;
        }
        free(tmp);

        snprintf(report->backup_path, sizeof report->backup_path, "%s", plan->backup_path);
        report->backup_bytes = plan->backup_len;
        report->backed_up = 1;
    }

    /* 2. Only now the savefile itself. */
    if (write_verified(plan->save_path, plan->save, plan->save_len, err, errsz) != 0)
        return -1;
    plan->applied = 1;

    /* 3. One form must not shadow the other: the game reads the gzipped file
     *    first, so leaving the one we did not write behind would undo all of
     *    this at the game's next launch. */
    if (plan->other_path && strcmp(plan->other_path, plan->save_path) != 0 &&
        plat_is_file(plan->other_path)) {
        if (plat_remove_file(plan->other_path) != 0) {
            err_set(err, errsz, "the savefile was replaced but '%s' could not be removed, "
                                "and the game would go on reading that one instead",
                    plan->other_path);
            save_plan_undo(plan);
            return -1;
        }
        snprintf(report->removed_path, sizeof report->removed_path, "%s", plan->other_path);
    }

    snprintf(report->source_path, sizeof report->source_path, "%s", plan->source_path);
    snprintf(report->save_path, sizeof report->save_path, "%s", plan->save_path);
    report->save_bytes = plan->save_len;
    report->used_fresh = plan->used_fresh;
    report->gzipped = gz_is_gzip(plan->save, plan->save_len);
    report->compressed = plan->compressed;
    return 0;
}

int save_plan_undo(save_plan *plan)
{
    char err[TB_ERR_LEN];
    unsigned char *prev = NULL;
    size_t prev_len = 0;
    int rc = 0;

    if (!plan->applied)
        return 0;

    if (!plan->backup) {
        /* There was no savefile before this, so there should be none after. */
        rc = plat_remove_file(plan->save_path);
    } else {
        /* Take the previous save straight back out of the archive we built. */
        if (archive_extract_save(plan->backup_path, &prev, &prev_len, err, sizeof err) != 0)
            return -1;
        rc = plat_write_file(plan->live_path, prev, prev_len);
        free(prev);
        if (rc == 0 && strcmp(plan->live_path, plan->save_path) != 0)
            plat_remove_file(plan->save_path);
    }

    if (rc == 0)
        plan->applied = 0;
    return rc;
}

void save_plan_free(save_plan *plan)
{
    free(plan->dir);
    free(plan->live_path);
    free(plan->entry_name);
    free(plan->backup_path);
    free(plan->backup);
    free(plan->source_path);
    free(plan->save);
    free(plan->save_path);
    free(plan->other_path);
    memset(plan, 0, sizeof(*plan));
}
