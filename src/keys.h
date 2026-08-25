/*
 * keys.h - Binding several players' controls to the same keys.
 *
 * Some custom tabs — Duality above all — ship co-op maps meant for one player
 * driving two ninjas at once, which needs both players bound to the same keys.
 * The game will not do that from its own options screen: a key already taken
 * is unbound from whoever had it. Written straight into the file, though, it
 * works, and the file is
 *
 *   <personal directory>/keys.vars
 *
 * a list of "name = value;" settings, one per line, with // comments and blank
 * lines between the blocks. A value is either KEYBIND("<key>") or -1, which
 * means the action has no key at all. Nothing else here cares what a value
 * says: they are copied and compared as they stand.
 *
 * Only three settings per player actually drive a ninja — left, right and jump
 * — and only those are touched. The rest work the menus and are none of our
 * business.
 *
 * Whatever is overwritten is copied into the state file first (see config.h,
 * "keybindings"), so unbinding puts back exactly what was there, -1 included.
 * A binding that is not in that record was never changed by us: unbinding
 * leaves it alone, unless the caller names its player, in which case the three
 * are cleared to -1 as the best that can be done without knowing the original.
 *
 * A tab that needs this says so in the digest, and installing it then binds
 * the players it names as if `bind` had been run by hand (see
 * keys_players_wanted); uninstalling puts them back.
 */
#ifndef TABBER_KEYS_H
#define TABBER_KEYS_H

#include <stddef.h>

#include "digest.h"
#include "json.h"
#include "paths.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bindings file, beside the savefile in the personal directory. */
#define KEYS_FILE_NAME      "keys.vars"

/* Digest key naming the players a tab wants bound, under its "disk" object. */
#define KJK_BIND            "bind"

/* How a setting is written, and what an unbound one reads. */
#define KEYS_ASSIGN         '='
#define KEYS_TERMINATOR     ';'
#define KEYS_UNBOUND        "-1"

/* The players the game supports, and how their settings are named. */
#define KEYS_PLAYER_MIN     1
#define KEYS_PLAYER_MAX     4
#define KEYS_NAME_PREFIX    "input_p%d_"
#define KEYS_NAME_FMT       KEYS_NAME_PREFIX "%s_key"
#define KEYS_LIST_SEP       ','

/* The three actions that move a ninja; everything else drives the menus. */
#define KEYS_ACTION_COUNT   3
extern const char *const keys_actions[KEYS_ACTION_COUNT];

/* The name of one player's setting for one action. Caller frees. */
char *keys_setting_name(int player, const char *action);

/*
 * Reads "1,2,3" into `players`, which must hold KEYS_PLAYER_MAX entries.
 * Whitespace is stripped, a player named twice is only taken once, and the
 * order is the caller's: the first is the one whose keys the others copy.
 * Returns 0, or -1 with a reason in `err`.
 */
int keys_players_parse(const char *text, int *players, size_t *count,
                       char *err, size_t errsz);

/*
 * The same list, read from a tab's digest entry instead of the command line:
 * "bind": [1, 2] under its "disk" object means player 2 is to answer to
 * player 1's keys while that tab is installed. `players` must hold
 * KEYS_PLAYER_MAX entries. Returns 0, with `count` at 0 when the tab asks for
 * nothing, or -1 with a reason in `err` when the digest names something that
 * is not a player.
 */
int keys_players_wanted(const npp_tab *tab, int *players, size_t *count,
                        char *err, size_t errsz);

/* ---- What was done ----------------------------------------------------- */

typedef enum {
    KEY_BOUND,      /* set to the key another player uses      */
    KEY_RESTORED,   /* put back to what the record says it was */
    KEY_CLEARED,    /* set to -1, the original being unknown   */
    KEY_SAME,       /* it already read that way                */
    KEY_ABSENT      /* the file carries no such setting        */
} key_outcome;

const char *key_outcome_text(key_outcome outcome);

typedef struct {
    char *name;             /* the setting, e.g. "input_p2_jump_key" */
    char *before, *after;   /* what it read, and what it reads now   */
    int player;
    key_outcome outcome;
} key_item;

typedef struct {
    char *path;             /* the keys.vars that was worked on        */
    key_item *items;
    size_t count;
    size_t changed;         /* settings actually written               */
    int source;             /* player the keys came from, 0 when undoing */
    int restoring;
} keys_report;

void keys_report_free(keys_report *report);

/* ---- The change, worked out before anything is written ----------------- */

typedef struct keys_change keys_change;

typedef struct {
    char *path;
    char *original;         /* the file as it was, put back by undo */
    size_t original_len;
    char *updated;          /* what it will hold, NULL when nothing changes */
    size_t updated_len;
    keys_change *items;
    size_t count;
    json_value *record;     /* the record to keep once this is applied */
    int source;
    int restoring;
    int applied;
} keys_plan;

/*
 * Works out binding `players[1..]` to the keys `players[0]` uses. `record` is
 * what the state file already holds, so an original recorded by an earlier
 * bind is never overwritten by the value that bind put there. Nothing is
 * written. Returns 0, or -1 with a reason in `err`.
 */
int keys_bind_build(const npp_paths *paths, const int *players, size_t count,
                    const json_value *record, keys_plan *plan, char *err, size_t errsz);

/*
 * The other direction: puts back everything `record` holds. Players named in
 * `players` (which may be empty) whose bindings the record does not cover have
 * their three settings cleared to -1 instead. Returns 0, or -1 with a reason
 * in `err`.
 */
int keys_unbind_build(const npp_paths *paths, const int *players, size_t count,
                      const json_value *record, keys_plan *plan, char *err, size_t errsz);

/* Writes the file. Returns 0, or -1 with a reason in `err` and nothing changed. */
int keys_plan_apply(keys_plan *plan, keys_report *report, char *err, size_t errsz);

/* Puts the original bytes back. */
void keys_plan_undo(keys_plan *plan);

/*
 * The record to store afterwards: for a bind, the originals it overwrote
 * merged with those already on record; for an unbind, an empty one, since
 * nothing is changed any more. Ownership passes to the caller.
 */
json_value *keys_plan_take_record(keys_plan *plan);

void keys_plan_free(keys_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_KEYS_H */
