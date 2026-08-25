/*
 * loc.h - Replacing in-game texts.
 *
 * Every string the game shows lives in one table,
 *
 *   <installation>/NPP/loc.txt
 *
 * one string per line, one language per field, fields separated by vertical
 * bars. The first field of a line is the LOC_ID naming the string, and the
 * first line is the header: its fields, after the LOC_ID column, name the
 * languages in the order every other line lists them.
 *
 *   LOC_ID|english|french|italian|...
 *   HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT|Friends|Amis|Amici|...
 *
 * A custom tab changes a handful of those strings: the friend highscore panel
 * becomes the speedrun boards it really shows, and the title screen names the
 * tab. The replacements are listed in loc_replacements below and are all in
 * English, whichever language they are written into; --languages decides which
 * columns are touched.
 *
 * Nothing is hardcoded about the originals. What a replacement overwrites is
 * copied into the state file first (see config.h, "strings"), so undoing the
 * change is a matter of putting back what is recorded there rather than of
 * carrying a copy of the game's own text around. The one exception is the
 * three English strings tabber's predecessor replaced without recording
 * anything: those originals are below, so a tab installed by the old tool can
 * still be undone by this one.
 *
 * The whole file is rewritten in one go, from a copy held in memory, so an
 * install that fails afterwards can put the original bytes straight back.
 */
#ifndef TABBER_LOC_H
#define TABBER_LOC_H

#include <stddef.h>

#include "digest.h"
#include "json.h"
#include "paths.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The game's string table, in the assets folder beside the levels. */
#define LOC_FILE_NAME       "loc.txt"

/* How it is laid out. */
#define LOC_FIELD_SEP       '|'
#define LOC_HEADER_ID       "LOC_ID"   /* first field of the first line */

/* What --languages accepts besides a comma-separated list. */
#define LOC_LANGS_ALL_WORD  "all"
#define LOC_LANGS_NONE_WORD "none"
#define LOC_LANGS_SEP       ','

/* ---- Which languages to write ------------------------------------------ */

typedef enum {
    LOC_LANGS_ALL,    /* every language the file carries (the default) */
    LOC_LANGS_NONE,   /* leave the texts alone                         */
    LOC_LANGS_SOME    /* the ones named below                          */
} loc_langs_kind;

typedef struct {
    loc_langs_kind kind;
    str_list names;   /* LOC_LANGS_SOME: trimmed, as the user spelled them */
} loc_langs;

/*
 * Parses "all", "none" or "spanish, english". Whitespace around each name is
 * stripped and matching is case-insensitive. Returns 0, or -1 when the value
 * names no language at all ("", ",,").
 */
int loc_langs_parse(const char *text, loc_langs *out);

void loc_langs_free(loc_langs *langs);

/* ---- What to replace --------------------------------------------------- */

/* Where a replacement's text comes from. */
typedef enum {
    LOC_TEXT_LITERAL,   /* the string below, as it stands */
    LOC_TEXT_TAB_NAME   /* the tab's name, capitalised    */
} loc_text_kind;

typedef struct {
    const char *id;         /* LOC_ID of the line to rewrite                 */
    loc_text_kind kind;
    const char *text;       /* the replacement, for LOC_TEXT_LITERAL         */
    const char *legacy;     /* English original the old installer overwrote,
                             * NULL for a replacement it never made          */
} loc_replacement;

#define LOC_REPLACEMENT_COUNT 3
extern const loc_replacement loc_replacements[LOC_REPLACEMENT_COUNT];

/* The only language the old installer touched, and so the only one its
 * originals above can be used to restore. */
#define LOC_LEGACY_LANG     "english"

/* The text `rep` calls for when installing `tab`. Caller frees. */
char *loc_replacement_text(const loc_replacement *rep, const npp_tab *tab);

/* ---- What was done ----------------------------------------------------- */

typedef enum {
    LOC_CHANGED,   /* the line was rewritten                        */
    LOC_ALREADY,   /* every language of it already read that way    */
    LOC_ABSENT     /* the file carries no line with that LOC_ID     */
} loc_outcome;

const char *loc_outcome_text(loc_outcome outcome);

typedef struct {
    char *id;
    char *text;            /* what it now reads, NULL when restoring, since
                            * each language gets its own original back      */
    size_t changed;        /* language fields written                       */
    size_t already;        /* ...that already read that way                 */
    loc_outcome outcome;
} loc_item;

typedef struct {
    char *path;            /* the loc.txt that was worked on   */
    loc_item *items;
    size_t count;
    size_t changed;        /* fields written, across every line */
    str_list languages;    /* the languages worked on, as the file spells them */
    str_list unknown;      /* asked for, but not in the file    */
    int restoring;         /* originals put back, not replaced  */
} loc_report;

void loc_report_free(loc_report *report);

/* ---- The change, worked out before anything is written ----------------- */

typedef struct loc_change loc_change;

typedef struct {
    char *path;
    char *original;        /* the file as it was, put back by undo */
    size_t original_len;
    char *updated;         /* what it will hold, NULL when nothing changes */
    size_t updated_len;
    loc_change *items;
    size_t count;
    str_list languages, unknown;
    json_value *record;    /* the originals, as the state file keeps them */
    int restoring;
    int applied;
} loc_plan;

/*
 * Works out the replacements a tab calls for: which languages the file has,
 * which of the wanted ones it does not, and what each line would read. Nothing
 * is written. Returns 0, or -1 with a reason in `err` when the file is not
 * there or does not look like the game's. Asking for no language at all yields
 * an empty plan, which applies cleanly and leaves the file untouched.
 */
int loc_plan_build(const npp_paths *paths, const npp_tab *tab, const loc_langs *langs,
                   loc_plan *plan, char *err, size_t errsz);

/*
 * The other direction: puts back the originals `record` holds, which is what
 * the state file's "strings" carries. Anything the record does not cover is
 * still checked against the originals the old installer overwrote, so texts it
 * left behind are restored too. Returns 0, or -1 with a reason in `err`.
 */
int loc_restore_build(const npp_paths *paths, const json_value *record,
                      loc_plan *plan, char *err, size_t errsz);

/* Writes the file. Returns 0, or -1 with a reason in `err` and nothing changed. */
int loc_plan_apply(loc_plan *plan, loc_report *report, char *err, size_t errsz);

/* Puts the original bytes back. */
void loc_plan_undo(loc_plan *plan);

void loc_plan_free(loc_plan *plan);

/*
 * The originals this plan overwrote, in the shape the state file keeps them:
 * one member per LOC_ID, holding one member per language changed. Ownership
 * passes to the caller, and the plan gives it up.
 */
json_value *loc_plan_take_record(loc_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_LOC_H */
