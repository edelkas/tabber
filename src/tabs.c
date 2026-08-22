#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "md5.h"
#include "net.h"
#include "palettes.h"
#include "platform.h"
#include "tabs.h"
#include "util.h"
#include "zip.h"

/* One entry decompressed in memory, waiting to be written out. */
typedef struct {
    const zip_entry *entry;
    unsigned char *data;
} staged_file;

char *tabs_root_dir(void)
{
    char *dir = plat_app_root();
    char *root = path_join(dir ? dir : ".", TABS_DIR_NAME);

    free(dir);
    return root;
}

char *tab_dir_path(const char *code)
{
    char *root = tabs_root_dir();
    char *lower = str_dup_lower(code);   /* the store is lowercase everywhere */
    char *dir = path_join(root, lower);

    free(root);
    free(lower);
    return dir;
}

int tab_is_downloaded(const char *code)
{
    char *dir;
    int present;

    if (!tab_code_is_valid(code))
        return 0;
    dir = tab_dir_path(code);
    present = plat_is_dir(dir);
    free(dir);
    return present;
}

int tab_code_is_valid(const char *code)
{
    size_t i;

    if (!code || !*code)
        return 0;
    for (i = 0; code[i]; i++) {
        int alpha = (code[i] >= 'a' && code[i] <= 'z') || (code[i] >= 'A' && code[i] <= 'Z');
        int digit = code[i] >= '0' && code[i] <= '9';
        if (!alpha && !digit)
            return 0;
    }
    return i <= TAB_CODE_MAX_LEN;
}

/* ---- Verification ------------------------------------------------------ */

/*
 * Checks that every file the digest lists under `key` is present in the
 * archive inside the levels folder. Returns the number found, or -1 when any
 * is missing, naming the offenders in `err`.
 */
static long verify_file_list(const zip_archive *zip, const json_value *disk,
                             const char *key, const char *levels_dir,
                             char *err, size_t errsz)
{
    const json_value *list = json_get(disk, key);
    const json_value *item;
    byte_buf missing = {0};
    long found = 0, missing_count = 0;

    if (!list || list->type != JSON_ARRAY)
        return 0;   /* nothing promised, nothing to check */

    for (item = list->children; item; item = item->next) {
        char *path;

        if (item->type != JSON_STRING)
            continue;
        path = str_fmt("%s/%s", levels_dir, item->string);
        if (zip_find(zip, path)) {
            found++;
        } else {
            /* Collect the first few names so the error stays readable. */
            if (missing_count < TABS_MAX_REPORTED) {
                if (missing_count > 0)
                    buf_append(&missing, ", ", 2);
                buf_append(&missing, path, strlen(path));
            }
            missing_count++;
        }
        free(path);
    }

    if (missing_count > 0) {
        char *names = buf_finish(&missing, NULL);
        if (missing_count > TABS_MAX_REPORTED)
            err_set(err, errsz, "%ld file(s) missing from the archive: %s, ...",
                    missing_count, names);
        else
            err_set(err, errsz, "%ld file(s) missing from the archive: %s",
                    missing_count, names);
        free(names);
        return -1;
    }

    buf_free(&missing);
    return found;
}

/* ---- Writing to disk --------------------------------------------------- */

/* Recreates the directory the entry lives in, then writes its contents. */
static int write_staged_file(const char *tab_dir, const staged_file *file,
                             char *err, size_t errsz)
{
    char *full = path_join(tab_dir, file->entry->name);
    char *parent;
    int rc = -1;

    path_to_native(full);
    parent = path_dirname(full);
    if (plat_mkdir_p(parent) != 0) {
        err_set(err, errsz, "cannot create '%s'", parent);
        goto done;
    }
    if (plat_write_file(full, file->data, file->entry->uncomp_size) != 0) {
        err_set(err, errsz, "cannot write '%s'", full);
        goto done;
    }
    rc = 0;

done:
    free(full);
    free(parent);
    return rc;
}

/* ASCII case-insensitive prefix test, for archive paths. */
static int name_has_prefix(const char *name, const char *prefix)
{
    size_t i;

    for (i = 0; prefix[i]; i++) {
        char a = name[i], b = prefix[i];

        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/*
 * Every palette the digest promises must have a folder of its own with
 * something in it. Checking here means an install never trips over a palette
 * that was missing from the download all along.
 */
static long verify_palettes(const zip_archive *zip, const json_value *disk,
                            const char *palettes_dir, char *err, size_t errsz)
{
    const json_value *list = json_get(disk, PJK_PALETTES);
    const json_value *item;
    long found = 0;

    if (!list || list->type != JSON_ARRAY)
        return 0;

    for (item = list->children; item; item = item->next) {
        char *prefix;
        size_t i;
        int any = 0;

        if (item->type != JSON_STRING)
            continue;
        prefix = str_fmt("%s/%s/", palettes_dir, item->string);
        for (i = 0; i < zip->count && !any; i++)
            any = !zip->entries[i].is_dir && name_has_prefix(zip->entries[i].name, prefix);
        if (!any) {
            err_set(err, errsz, "the archive has no files under '%s'", prefix);
            free(prefix);
            return -1;
        }
        free(prefix);
        found++;
    }
    return found;
}

/* ---- State ------------------------------------------------------------- */

/*
 * Records the download in the tool's state file. A problem here does not undo
 * a good installation, so it is reported as a warning rather than a failure.
 */
static void record_download(const npp_tab *tab, tab_report *report)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);

    if (!cfg) {
        err_set(report->warning, sizeof report->warning,
                "the download was not recorded: %s", err);
        return;
    }

    config_set_downloaded(cfg, tab->id, tab->code);
    if (config_save(cfg, err, sizeof err) != 0)
        err_set(report->warning, sizeof report->warning,
                "the download was not recorded: %s", err);
    else
        snprintf(report->state_path, sizeof report->state_path, "%s", cfg->path);

    config_free(cfg);
}

/* ---- Fetch ------------------------------------------------------------- */

int tab_fetch(const digest *dig, const npp_tab *tab, tab_report *report,
              char *err, size_t errsz)
{
    const json_value *download, *disk;
    const char *link, *expected_md5, *levels_dir;
    long expected_size, expected_disk_size, level_files, challenge_files, palettes;
    char *archive = NULL, *dir = NULL;
    size_t archive_len = 0, i;
    zip_archive zip;
    staged_file *staged = NULL;
    size_t staged_count = 0;
    int zip_open_ok = 0, rc = -1;

    memset(report, 0, sizeof(*report));
    memset(&zip, 0, sizeof(zip));

    /* --- What the digest promises --- */
    download = json_get(tab->node, TJK_DOWNLOAD);
    disk = json_get(tab->node, TJK_DISK);
    link = json_get_string(download, TJK_LINK, NULL);
    expected_md5 = json_get_string(download, TJK_MD5, NULL);
    expected_size = json_get_int(download, TJK_SIZE, -1);
    expected_disk_size = json_get_int(disk, TJK_SIZE, -1);
    levels_dir = digest_levels_dir(dig);

    if (!link) {
        err_set(err, errsz, "the digest has no download link for '%s'", tab->code);
        return -1;
    }

    /* --- Download --- */
    if (net_fetch(link, &archive, &archive_len, err, errsz) != 0)
        goto done;

    /* --- Size --- */
    if (expected_size >= 0 && (size_t)expected_size != archive_len) {
        err_set(err, errsz, "size mismatch: the digest says %ld bytes, the download is %lu",
                expected_size, (unsigned long)archive_len);
        goto done;
    }

    /* --- MD5 --- */
    md5_hex(archive, archive_len, report->md5);
    if (expected_md5 && !str_ieq(report->md5, expected_md5)) {
        err_set(err, errsz, "MD5 mismatch: the digest says %s, the download is %s",
                expected_md5, report->md5);
        goto done;
    }

    /* --- Archive structure --- */
    if (zip_open(&zip, archive, archive_len, err, errsz) != 0)
        goto done;
    zip_open_ok = 1;

    if (expected_disk_size >= 0 && (size_t)expected_disk_size != zip_total_uncompressed(&zip)) {
        err_set(err, errsz, "uncompressed size mismatch: the digest says %ld bytes, the archive holds %lu",
                expected_disk_size, (unsigned long)zip_total_uncompressed(&zip));
        goto done;
    }

    /* --- Promised contents --- */
    level_files = verify_file_list(&zip, disk, TJK_LEVEL_FILES, levels_dir, err, errsz);
    if (level_files < 0)
        goto done;
    challenge_files = verify_file_list(&zip, disk, TJK_CHALLENGE_FILES, levels_dir, err, errsz);
    if (challenge_files < 0)
        goto done;
    palettes = verify_palettes(&zip, disk, digest_palettes_dir(dig), err, errsz);
    if (palettes < 0)
        goto done;

    /* --- Decompress everything before touching the disk --- */
    staged = zip.count ? xmalloc(zip.count * sizeof(*staged)) : NULL;
    for (i = 0; i < zip.count; i++) {
        const zip_entry *entry = &zip.entries[i];

        if (!zip_name_is_safe(entry->name)) {
            err_set(err, errsz, "the archive holds an unsafe path: '%s'", entry->name);
            goto done;
        }
        if (entry->is_dir)
            continue;

        staged[staged_count].entry = entry;
        staged[staged_count].data = zip_read(&zip, entry, err, errsz);
        if (!staged[staged_count].data)
            goto done;   /* CRC or decompression failure */
        staged_count++;
    }

    /* --- Persist --- */
    dir = tab_dir_path(tab->code);
    if (plat_remove_tree(dir) != 0) {     /* replace any previous copy */
        err_set(err, errsz, "cannot clear the existing '%s'", dir);
        goto done;
    }
    if (plat_mkdir_p(dir) != 0) {
        err_set(err, errsz, "cannot create '%s'", dir);
        goto done;
    }

    /* Empty directories in the archive are recreated too, so the layout matches. */
    for (i = 0; i < zip.count; i++) {
        char *sub;

        if (!zip.entries[i].is_dir)
            continue;
        sub = path_join(dir, zip.entries[i].name);
        path_to_native(sub);
        if (plat_mkdir_p(sub) != 0) {
            err_set(err, errsz, "cannot create '%s'", sub);
            free(sub);
            plat_remove_tree(dir);
            goto done;
        }
        free(sub);
    }

    for (i = 0; i < staged_count; i++) {
        if (write_staged_file(dir, &staged[i], err, errsz) != 0) {
            plat_remove_tree(dir);   /* leave nothing half-installed */
            goto done;
        }
    }

    report->dir = dir;
    dir = NULL;                      /* ownership moves to the report */
    report->link = link;
    report->zip_bytes = archive_len;
    report->disk_bytes = zip_total_uncompressed(&zip);
    report->entry_count = zip.count;
    report->file_count = staged_count;
    report->level_files = (size_t)level_files;
    report->challenge_files = (size_t)challenge_files;
    report->palettes = (size_t)palettes;
    record_download(tab, report);
    rc = 0;

done:
    for (i = 0; i < staged_count; i++)
        free(staged[i].data);
    free(staged);
    if (zip_open_ok)
        zip_close(&zip);
    free(archive);
    free(dir);
    return rc;
}

void tab_report_free(tab_report *report)
{
    free(report->dir);
    report->dir = NULL;
}

/* ---- Remove ------------------------------------------------------------ */

int tab_remove(const char *code, int id, tab_remove_report *report,
               char *err, size_t errsz)
{
    char cfg_err[TB_ERR_LEN];
    config *cfg;
    char *dir;

    memset(report, 0, sizeof(*report));

    /* The code becomes a directory we delete recursively, so vet it first. */
    if (!tab_code_is_valid(code)) {
        err_set(err, errsz, "'%s' is not a valid tab code", code ? code : "");
        return -1;
    }

    dir = tab_dir_path(code);
    report->had_files = plat_is_dir(dir);

    if (report->had_files && plat_remove_tree(dir) != 0) {
        err_set(err, errsz, "cannot remove '%s'; is a file in it open?", dir);
        free(dir);
        return -1;
    }

    /* Keep the entry: its history is worth having if the tab comes back. */
    cfg = config_load(cfg_err, sizeof cfg_err);
    if (!cfg) {
        err_set(report->warning, sizeof report->warning,
                "the removal was not recorded: %s", cfg_err);
    } else {
        /*
         * Only record something that actually happened: either files were
         * deleted, or the state claimed the tab was downloaded and its files
         * had gone missing behind our back. Removing an already-removed tab
         * changes nothing and must not restamp the date.
         */
        const json_value *entry = config_find_tab(cfg, code);
        int claimed = entry && json_get_bool(entry, CJK_DOWNLOADED, 0);

        if (report->had_files || claimed)
            report->recorded = config_set_removed(cfg, id, code);
        if (report->recorded) {
            if (config_save(cfg, cfg_err, sizeof cfg_err) != 0) {
                err_set(report->warning, sizeof report->warning,
                        "the removal was not recorded: %s", cfg_err);
                report->recorded = 0;
            } else {
                snprintf(report->state_path, sizeof report->state_path, "%s", cfg->path);
            }
        }
        config_free(cfg);
    }

    report->dir = dir;
    return 0;
}

void tab_remove_report_free(tab_remove_report *report)
{
    free(report->dir);
    report->dir = NULL;
}
