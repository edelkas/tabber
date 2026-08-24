/*
 * save.h - Swapping N++'s savefile when a custom tab goes in or comes out.
 *
 * This is the only part of tabber that touches something the user cannot get
 * back: the game's files can always be restored by Steam, a savefile cannot.
 * Everything here is therefore built around one rule: the save that is on disk
 * is never overwritten until a verified copy of it is safely archived.
 *
 * Layout, kept byte-compatible with the previous per-tab installers so both
 * can be used on the same machine:
 *
 *   nprofile[.gz]           the live savefile, in N++'s personal folder
 *   nprofile_original.zip   the vanilla save, archived when a tab is installed
 *   nprofile_<code>.zip     that tab's save, archived when it is uninstalled
 *
 * The archives are ordinary ZIPs holding a single entry named after the file
 * that went in ("nprofile" or "nprofile.gz"), exactly as the old installers
 * wrote them, and the save inside a fresh install comes from the ZIP tabber
 * ships (see save_fresh_path).
 *
 * Compression: since TEN++ the game stores the save gzipped, and falls back to
 * the uncompressed file when the gzipped one is absent. Older builds know only
 * the uncompressed form. Because we cannot tell which build the user has, we
 * read whichever is there (preferring the gzipped one) and never *create* a
 * gzipped save: a save is written back gzipped only when it already was and
 * the game has proven it understands the format by having one on disk. That
 * way an old build is never handed a file it cannot read, and a new build
 * simply falls back to the uncompressed one and re-compresses it on its next
 * save. Whichever form is written, the other is removed, so the game cannot
 * end up reading the one we did not mean it to.
 *
 * SAVE_FORCE_COMPRESS relaxes only that last part: when the game has proven it
 * reads gzip, an uncompressed save is compressed on the way in rather than
 * left for the game to fall back to. Anything compressed here is unpacked
 * again and compared before it is written, so a save the game could not read
 * is caught here rather than at its next launch.
 */
#ifndef TABBER_SAVE_H
#define TABBER_SAVE_H

#include <stddef.h>

#include "paths.h"
#include "util.h"

/* The savefile and its archives, all in N++'s personal folder. */
#define SAVE_NAME            "nprofile"
#define SAVE_GZ_NAME         SAVE_NAME ".gz"
#define SAVE_BACKUP_ORIGINAL SAVE_NAME "_original.zip"
#define SAVE_BACKUP_FMT      SAVE_NAME "_%s.zip"      /* %s: the tab code, lowercase */

/* The save tabber ships for a tab that has never been played. */
#define SAVE_FRESH_ZIP       SAVE_NAME ".zip"
#define SAVE_RES_DIR         "res"

/*
 * ...which normally comes from inside the executable (see resource.h), and is
 * named rather than pathed in reports, there being no file to point at.
 */
#define SAVE_FRESH_BUILTIN   "the fresh save built into tabber"

/* Entry looked up inside any of those archives, as a prefix ("nprofile*"). */
#define SAVE_ENTRY_PREFIX    SAVE_NAME

/* Suffix of the temporary files written before a rename puts them in place. */
#define SAVE_TMP_SUFFIX      ".tabber-tmp"

/* Written and deleted to confirm the personal folder accepts changes. */
#define SAVE_PROBE_FILE      ".tabber-save-test"

/*
 * Options for a swap. By default a save is only written gzipped when it
 * already was; SAVE_FORCE_COMPRESS also compresses one that is not, provided
 * the game has shown it reads gzip by having a gzipped save of its own. It
 * costs the compression to save the game a fallback it handles anyway.
 */
#define SAVE_FORCE_COMPRESS  0x01u

/* Points at a ready-made fresh save instead of the shipped one, for tests. */
#define TABBER_ENV_FRESH_SAVE "TABBER_FRESH_SAVE"

/* Which form the savefile is in. */
typedef enum {
    SAVE_ABSENT,     /* no savefile at all: the game has never been run */
    SAVE_RAW,        /* nprofile, uncompressed                          */
    SAVE_GZIPPED     /* nprofile.gz, as TEN++ and later write it        */
} save_form;

/* What a swap did, for reporting. */
typedef struct {
    char backup_path[512];   /* archive written, empty if there was nothing to archive */
    char source_path[512];   /* archive the new save came from                         */
    char save_path[512];     /* savefile written                                       */
    char removed_path[512];  /* the other form, deleted afterwards; empty if none      */
    size_t backup_bytes;
    size_t save_bytes;
    int backed_up;           /* a save was archived                                    */
    int used_fresh;          /* the new save came from the shipped archive             */
    int from_builtin;        /* ...the one inside the executable, not a file            */
    int gzipped;             /* the save was written gzipped                           */
    int compressed;          /* ...and tabber was the one that compressed it           */
} save_report;

/*
 * A swap worked out in full, in memory, before anything is written. Building
 * one reads and verifies every input; applying one writes them out.
 */
typedef struct {
    char *dir;                 /* N++'s personal folder                    */
    save_form form;            /* the live save's form before the swap     */
    char *live_path;           /* the live save, NULL when there is none   */
    size_t live_len;           /* its size, checked again after archiving  */
    char *entry_name;          /* its name inside the archive              */
    char *backup_path;         /* archive to write                         */
    unsigned char *backup;     /* its bytes, NULL when nothing to archive  */
    size_t backup_len;
    char *source_path;         /* archive the new save comes from          */
    int used_fresh;
    int from_builtin;          /* it is the embedded one, so not a path    */
    unsigned char *save;       /* the new save, ready to write             */
    size_t save_len;
    char *save_path;           /* where it goes                            */
    char *other_path;          /* the form to delete once it is in place   */
    int compressed;            /* the new save was gzipped by us           */
    int applied;               /* the new save is on disk                  */
} save_plan;

/*
 * Path of a fresh save on disk, or NULL when there is none: the environment
 * override first, then res/ beside the executable. Without one the copy built
 * into the binary is used, so this returning NULL is the ordinary case.
 */
char *save_fresh_path(void);

/*
 * Works out the whole swap without touching the disk except to read: which
 * save is live, what its archive will contain, where the new save comes from
 * and which form it has to be written in. `installing` picks the direction:
 * vanilla save out and the tab's (or a fresh) save in, or the reverse.
 * Returns 0 on success, -1 with a reason in `err` and nothing changed.
 */
int save_plan_build(const npp_paths *paths, const char *code, int installing,
                    unsigned flags, save_plan *plan, char *err, size_t errsz);

/*
 * Writes the plan: the archive first, verified by reading it back, and only
 * then the new savefile. Returns 0 on success, -1 with a reason in `err`, in
 * which case the live savefile is still the one that was there.
 */
int save_plan_apply(save_plan *plan, save_report *report, char *err, size_t errsz);

/*
 * Puts the previous save back from the archive this plan made, for when a
 * later step of an install or uninstall fails. Does nothing if the plan was
 * never applied. Returns 0 on success.
 */
int save_plan_undo(save_plan *plan);

void save_plan_free(save_plan *plan);

#endif /* TABBER_SAVE_H */
