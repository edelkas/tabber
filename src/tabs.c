#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "md5.h"
#include "net.h"
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
    char *dir = plat_exe_dir();
    char *root = path_join(dir ? dir : ".", TABS_DIR_NAME);

    free(dir);
    return root;
}

char *tab_dir_path(const char *code)
{
    char *root = tabs_root_dir();
    char *dir = path_join(root, code);

    free(root);
    return dir;
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

/* ---- Fetch ------------------------------------------------------------- */

int tab_fetch(const digest *dig, const npp_tab *tab, tab_report *report,
              char *err, size_t errsz)
{
    const json_value *download, *disk;
    const char *link, *expected_md5, *levels_dir;
    long expected_size, expected_disk_size, level_files, challenge_files;
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
