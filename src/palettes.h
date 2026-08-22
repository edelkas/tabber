/*
 * palettes.h - Installing the colour palettes a custom tab bundles.
 *
 * A palette is a folder of TGA colour swatches. The game reads them from
 *
 *   <installation>/NPP/Palettes/<palette name>/
 *
 * where every subfolder is one palette, named by the folder itself, and the
 * folder as a whole may not exist at all: on Windows the game ships without
 * it. Installing a tab's palettes is therefore no more than copying folders
 * across, and the whole of this module is about the three ways that can go
 * wrong.
 *
 *   1. The name is taken. Palette names have to be unique, and matching is
 *      case-insensitive so a name cannot be freed by changing its spelling.
 *      What to do is the caller's choice (see palette_collision).
 *
 *   2. The name is taken by a palette that is not in the folder. Most official
 *      palettes are baked into the library, take precedence over anything on
 *      disk, and cannot be seen by looking at the game's files. They are
 *      hardcoded below for that reason. Four of them were cut before release
 *      and were baked with " CUT" appended, so "line" is free to use while
 *      "line CUT" is not.
 *
 *   3. There is no room left. The game indexes palettes with a single byte, so
 *      it parses PALETTE_LIMIT of them and silently ignores the rest, baked
 *      ones included. Going over the line breaks nothing, but it would push
 *      somebody else's palettes out of the game, so a palette that does not
 *      fit is not copied at all. What counts towards the total is the baked
 *      ones plus the folders the game does not already skip: a folder named
 *      after a baked palette is dropped before parsing and is free.
 *
 * None of the three is an error: each palette gets its own line in the report
 * saying what became of it, and the install carries on.
 *
 * Uninstalling removes the palettes that went in, and only those: the names
 * are read back from the state file, so a palette that was skipped because the
 * user already had one of that name is never mistaken for ours.
 */
#ifndef TABBER_PALETTES_H
#define TABBER_PALETTES_H

#include <stddef.h>

#include "digest.h"
#include "paths.h"
#include "util.h"

/* Digest key naming the palettes a tab bundles, under its "disk" object. */
#define PJK_PALETTES         "palettes"

/*
 * Palettes the game parses at all, and how many of those are baked into the
 * library rather than sitting in the folder. The difference is what is left
 * for everyone else, custom palettes the game already ships included.
 */
#define PALETTE_LIMIT        256
#define PALETTE_BAKED_COUNT  123
#define PALETTE_MAX_CUSTOM   (PALETTE_LIMIT - PALETTE_BAKED_COUNT)

/* Appended to the baked name of the four palettes cut before release. */
#define PALETTE_CUT_SUFFIX   " CUT"

/* How a palette is renamed out of a collision, and how far we count. */
#define PALETTE_RENAME_FMT   "%s %d"
#define PALETTE_RENAME_MAX   99

/* An existing palette is moved aside under this suffix while one is replaced,
 * so a failure later in the install can still put it back. */
#define PALETTE_STASH_SUFFIX ".tabber-old"

/* The 123 baked palette names, in the order the game loads them. */
extern const char *const palette_baked_names[PALETTE_BAKED_COUNT];

/* Whether the library already bakes a palette of this name (case-insensitive). */
int palette_is_baked(const char *name);

/* What to do about a palette whose name is already in use. */
typedef enum {
    PALETTE_SKIP,      /* leave the one that is there, do not install ours */
    PALETTE_REPLACE,   /* overwrite it with the tab's                      */
    PALETTE_SUFFIX     /* install ours under "<name> 2", "<name> 3", ...   */
} palette_collision;

/* The mode named by `text` ("skip", "replace", "suffix"), or -1. */
int palette_collision_parse(const char *text, palette_collision *out);
const char *palette_collision_name(palette_collision mode);

/* What became of one palette. */
typedef enum {
    PAL_INSTALLED,      /* copied in under its own name                     */
    PAL_RENAMED,        /* copied in under a suffixed name                  */
    PAL_REPLACED,       /* copied in over a palette of the same name        */
    PAL_SKIPPED_PRESENT,/* the game folder already has one of that name     */
    PAL_SKIPPED_BAKED,  /* the library bakes that name and would win        */
    PAL_SKIPPED_DUP,    /* the tab bundles two palettes of the same name    */
    PAL_SKIPPED_FULL,   /* the game's palette limit left no room for it     */
    PAL_REMOVED,        /* deleted again when the tab was uninstalled       */
    PAL_ABSENT,         /* ...or was not there any more to delete           */
    PAL_KEPT,           /* deliberately left in the game folder             */
    PAL_FAILED          /* something went wrong; see the detail             */
} palette_outcome;

/* A short phrase describing an outcome, for the log. */
const char *palette_outcome_text(palette_outcome outcome);

typedef struct {
    char *name;                /* the palette as the tab names it            */
    char *target;              /* folder it took in the game, NULL when none */
    palette_outcome outcome;
    size_t files;              /* files copied into it                       */
    char detail[TB_ERR_LEN];   /* what went wrong, when something did        */
} palette_item;

typedef struct {
    char *dir;                 /* the game's palettes folder                 */
    palette_item *items;
    size_t count;              /* palettes considered                        */
    size_t installed;          /* ...of which went in                        */
    size_t skipped;
    size_t removed;
    size_t failed;
    size_t existing;           /* palettes in the folder that count, i.e.
                                * excluding those named after a baked one    */
    size_t total;              /* palettes the game sees afterwards, baked
                                * ones included                              */
} palette_report;

void palette_report_free(palette_report *report);

/* An installation worked out in full before anything is copied. */
typedef struct palette_entry palette_entry;

typedef struct {
    char *dir;                 /* the game's palettes folder     */
    char *source_dir;          /* the tab's palettes folder      */
    palette_entry *items;
    size_t count;
    size_t existing;           /* ...that count towards the limit */
    size_t adding;             /* folders this plan would create */
    palette_collision mode;
    int applied;
} palette_plan;

/*
 * Works out where each of the tab's palettes would go: which names are taken,
 * by the folder or by the library, and whether there is still room under the
 * game's limit. Nothing is written. A tab that bundles no palettes yields an
 * empty plan, which applies cleanly. Returns 0 on success, -1 with a reason in
 * `err` when the tab's own files are not where the digest says they are.
 */
int palettes_plan_build(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                        palette_collision mode, palette_plan *plan,
                        char *err, size_t errsz);

/*
 * Copies the palettes in. Under PALETTE_REPLACE the palette being overwritten
 * is moved aside first, so palettes_plan_undo can still put it back; it is
 * deleted for good by palettes_plan_commit. Returns 0 on success, -1 with a
 * reason in `err` and everything it had already copied undone.
 */
int palettes_plan_apply(palette_plan *plan, palette_report *report,
                        char *err, size_t errsz);

/* Deletes the palettes this plan copied in, and puts back any it replaced. */
void palettes_plan_undo(palette_plan *plan);

/* Past the point of undo: drops the copies kept of replaced palettes. */
void palettes_plan_commit(palette_plan *plan);

void palettes_plan_free(palette_plan *plan);

/* The folder names the plan actually created, for the state file to remember. */
void palettes_plan_installed(const palette_plan *plan, str_list *out);

/*
 * Deletes the palettes named in `names` from the game's folder, which is what
 * uninstalling a tab does with the ones it brought. `keep` reports them
 * without touching anything instead. A palette that is already gone is not an
 * error. Returns 0 unless the folder itself cannot be worked out.
 */
int palettes_remove(const digest *dig, const npp_paths *paths, const str_list *names,
                    int keep, palette_report *report, char *err, size_t errsz);

/* The palettes a tab bundles, as the digest lists them. */
void palettes_bundled(const npp_tab *tab, str_list *out);

#endif /* TABBER_PALETTES_H */
