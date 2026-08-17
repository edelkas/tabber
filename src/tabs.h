/*
 * tabs.h - Downloading, verifying and unpacking custom tabs.
 *
 * A tab is downloaded as a ZIP, checked entirely in memory (size, MD5, total
 * uncompressed size, per-entry CRC and the presence of the level/challenge
 * files the digest promises) and only then written to disk, under
 * <tool root>/tabs/<code>/. A tab that fails any check leaves nothing behind.
 */
#ifndef TABBER_TABS_H
#define TABBER_TABS_H

#include <stddef.h>

#include "digest.h"
#include "md5.h"
#include "util.h"

/* Local store: <directory of the executable>/tabs/<code>/ */
#define TABS_DIR_NAME       "tabs"

/* Digest keys describing the download and its unpacked contents. */
#define TJK_DOWNLOAD        "download"
#define TJK_LINK            "link"
#define TJK_SIZE            "size"
#define TJK_MD5             "md5"
#define TJK_DISK            "disk"
#define TJK_LEVEL_FILES     "level_files"
#define TJK_CHALLENGE_FILES "challenge_files"

/* How many missing files an error message names before summarising. */
#define TABS_MAX_REPORTED   5

/* What a successful fetch did, for the caller to report. */
typedef struct {
    char *dir;                     /* where the tab was written        */
    const char *link;              /* URL it came from                 */
    size_t zip_bytes;              /* size of the downloaded archive   */
    size_t disk_bytes;             /* total uncompressed size          */
    size_t entry_count;            /* entries in the archive           */
    size_t file_count;             /* files written to disk            */
    size_t level_files;            /* required level files found       */
    size_t challenge_files;        /* required challenge files found   */
    char md5[MD5_HEX_LEN + 1];     /* verified hash of the archive     */
    char state_path[512];          /* state file updated, empty if none */
    char warning[TB_ERR_LEN];      /* non-fatal problem, empty if none  */
} tab_report;

/*
 * Downloads, verifies and unpacks one tab. Returns 0 on success and fills
 * `report` (release it with tab_report_free). On failure returns -1, writes
 * the reason into `err` and leaves no partial installation behind.
 */
int tab_fetch(const digest *dig, const npp_tab *tab, tab_report *report,
              char *err, size_t errsz);

void tab_report_free(tab_report *report);

/* Root of the local tab store, and the directory of one tab. Caller frees. */
char *tabs_root_dir(void);
char *tab_dir_path(const char *code);

#endif /* TABBER_TABS_H */
