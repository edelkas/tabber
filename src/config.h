/*
 * config.h - The tool's own configuration and state, kept in config.json next
 * to the executable.
 *
 * The file is read into a JSON tree, modified in place and written back, so
 * keys this version does not know about (hand-edited settings, fields added by
 * a later version) survive untouched. Writes are staged and swapped in, like
 * the digest cache.
 *
 * The "tabs" array tracks what has happened to each custom tab:
 *
 *   {
 *     "id": 21,
 *     "code": "lit",
 *     "downloaded": true,
 *     "installed": false,
 *     "download_date": "2026-08-17T18:42:03Z",
 *     "install_date": null,
 *     "uninstall_date": null,
 *     "remove_date": null,
 *     "palettes": ["nova cosmic", "nova orbit"]
 *   }
 *
 * "palettes" holds the folders an install actually created in the game's
 * palettes folder, so uninstalling deletes those and no others: a palette that
 * was skipped because the user already had one of that name is not in the list
 * and is therefore never mistaken for the tab's.
 *
 * Alongside "tabs", at the root, "strings" records the in-game texts an install
 * replaced, holding the original of every language it wrote over:
 *
 *   "strings": {
 *     "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT": { "english": "Friends" }
 *   }
 *
 * Only one tab can be installed at a time, so one record is enough, and it
 * always describes the replacements that are live right now: uninstalling puts
 * the originals back and empties it. Keeping them here rather than in the
 * source is what lets tabber undo a change without knowing the game's own text.
 *
 * "keybindings", also at the root, does the same for the controls the `bind`
 * command changes — and an install does, when the tab's digest entry asks for
 * several players to share one set — one member per setting overwritten:
 *
 *   "keybindings": { "input_p2_jump_key": "KEYBIND(\"V\")" }
 *
 * `unbind` and uninstalling put those back and empty it, so it too always
 * describes what is live. A value of "-1" is a binding that had no key to
 * begin with, which is worth recording like any other.
 *
 * "update" holds what the last look for a newer tabber found, and the two
 * things the user gets to say about the looking — what to do about a release
 * when one turns up, and how far apart the looks are:
 *
 *   "update": { "policy": "auto", "interval_hours": 24, "check": true, ... }
 *
 * "gui", last, is the graphical front-end's own corner of the file — what the
 * window was left set to, which the command line neither reads nor writes:
 *
 *   "gui": { "theme": "light", "status_bar": true, "save_logs": true }
 */
#ifndef TABBER_CONFIG_H
#define TABBER_CONFIG_H

#include <stddef.h>

#include "json.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_FILENAME      "config.json"
#define CONFIG_TMP_SUFFIX    ".tmp"

/* Keys of the state file. */
#define CJK_TABS             "tabs"
#define CJK_STATE            "state"     /* what the game files look like */
#define CJK_LIBRARY          "library"   /* did the library check pass?   */
#define CJK_ID               "id"
#define CJK_CODE             "code"
#define CJK_DOWNLOADED       "downloaded"
#define CJK_INSTALLED        "installed"
#define CJK_DOWNLOAD_DATE    "download_date"
#define CJK_INSTALL_DATE     "install_date"
#define CJK_UNINSTALL_DATE   "uninstall_date"
#define CJK_REMOVE_DATE      "remove_date"
#define CJK_PALETTES         "palettes"   /* folders an install created    */
#define CJK_STRINGS          "strings"    /* in-game texts replaced, at the root */
#define CJK_KEYBINDINGS      "keybindings"/* controls changed, likewise          */
#define CJK_UPDATE           "update"     /* what the last version check found   */
#define CJK_CHECK            "check"      /* whether to look at all              */
#define CJK_POLICY           "policy"     /* ...what to do about what it finds   */
#define CJK_INTERVAL         "interval_hours" /* ...and how far apart the looks  */
#define CJK_LAST_CHECK       "last_check"
#define CJK_LATEST           "latest"     /* newest version the check has seen   */
#define CJK_DECLINED         "declined"   /* version the user said no to         */
#define CJK_APPLIED          "applied"    /* update installed, not yet announced */
#define CJK_GUI              "gui"        /* what the front-end was left set to  */
#define CJK_THEME            "theme"
#define CJK_STATUS_BAR       "status_bar" /* is the bar along the bottom shown?  */
#define CJK_SAVE_LOGS        "save_logs"  /* ...and is what it says kept on disk? */

/* The two themes the front-end draws itself in. Anything else, the absent key
 * included, is the dark one: it is what the program has always looked like. */
#define CONFIG_THEME_DARK    "dark"
#define CONFIG_THEME_LIGHT   "light"

typedef struct {
    json_value *root;   /* whole document, "tabs" included */
    char *path;
} config;

/* Path of the state file. Caller frees. */
char *config_path(void);

/*
 * Loads the state file, or starts an empty one when it does not exist yet.
 * Returns NULL with a reason in `err` if the file exists but cannot be used;
 * in that case nothing is overwritten, so a hand-edited file is never lost.
 */
config *config_load(char *err, size_t errsz);

/* Writes the state back out. Returns 0 on success. */
int config_save(config *cfg, char *err, size_t errsz);

void config_free(config *cfg);

/* The entry for `code`, or NULL when the tab has no history yet. */
json_value *config_find_tab(config *cfg, const char *code);

/*
 * Records whether the game's library still matches what we believe is
 * installed, under "state" -> "library". Refreshed on every check.
 */
void config_set_state_library(config *cfg, int ok);

/*
 * The entry for `code`, created with all fields at their defaults (not
 * downloaded, not installed, every date null) if it is not there yet.
 */
json_value *config_tab_entry(config *cfg, int id, const char *code);

/* Records that a tab has just been downloaded: sets the flag and the date. */
void config_set_downloaded(config *cfg, int id, const char *code);

/* Records that a tab has just been installed into the game. */
void config_set_installed(config *cfg, int id, const char *code);

/*
 * Records that a tab has just been uninstalled: clears "installed" and stamps
 * "uninstall_date". "install_date" is deliberately kept, so when the tab was
 * last installed is not lost.
 */
void config_set_uninstalled(config *cfg, int id, const char *code);

/*
 * Records that a tab's files have been removed: clears "downloaded" and stamps
 * "remove_date". The entry itself is kept, so a later re-download still has its
 * history. `id` may be -1 when unknown, in which case an entry is only updated,
 * never created. Returns 1 if anything was recorded.
 */
int config_set_removed(config *cfg, int id, const char *code);

/*
 * Records the palette folders an install created, replacing whatever was
 * listed before. An empty list clears the record, which is what uninstalling
 * does once they are gone.
 */
void config_set_palettes(config *cfg, int id, const char *code, const str_list *names);

/*
 * Reads that record back. Returns 1 when the tab has one at all, which an
 * install that put no palette in still does: an empty record means "none went
 * in", and is not the same as never having recorded any.
 */
int config_get_palettes(config *cfg, const char *code, str_list *out);

/*
 * Records the in-game texts an install replaced, taking ownership of `record`
 * and replacing whatever was there. NULL empties the record, which is what
 * uninstalling does once the originals are back.
 */
void config_set_strings(config *cfg, json_value *record);

/* That record, or NULL when the state file has none. Owned by the config. */
const json_value *config_get_strings(config *cfg);

/*
 * The same for the key bindings the `bind` command overwrote. NULL empties the
 * record, which is what `unbind` does once the originals are back.
 */
void config_set_keybindings(config *cfg, json_value *record);
const json_value *config_get_keybindings(config *cfg);

/* ---- Looking for a newer tabber ---------------------------------------- */

/*
 * What to do when a look turns up a release newer than the one running, as
 * the file spells it. Anything else in there, the absent key included, is the
 * first of them: a tool nobody has told otherwise keeps itself up to date.
 */
#define CONFIG_POLICY_AUTO    "auto"    /* take it, as soon as nothing is in the way */
#define CONFIG_POLICY_PROMPT  "prompt"  /* ask, once per version                     */
#define CONFIG_POLICY_NONE    "none"    /* say it is there and do no more            */

typedef enum {
    UPDATE_POLICY_AUTO, UPDATE_POLICY_PROMPT, UPDATE_POLICY_NONE
} update_policy;

/*
 * The closest together and the furthest apart two looks may be asked to
 * happen: a file asking for less than the one, more than the other, or for
 * nothing that is a number at all, is held to what it can have. The far end is
 * the largest the front-end's own control can be made to say, which is four
 * figures of days, and is there so that a number cannot be made big enough to
 * overflow the hours it is kept as.
 */
#define CONFIG_INTERVAL_MIN   1
#define CONFIG_INTERVAL_MAX   (9999 * 24)

/* Whether the user has left the check switched on. Absent means yes. */
int config_update_enabled(config *cfg);

/* What they have asked to happen when one finds something. */
update_policy config_update_policy(config *cfg);
void config_set_update_policy(config *cfg, update_policy policy);

/*
 * How long a look stays good for, in hours: what config_update_due is meant to
 * be handed, rather than a figure of the caller's own. UPDATE_CHECK_HOURS when
 * the file does not say, which is what both front-ends have always used.
 */
int config_update_interval(config *cfg);
void config_set_update_interval(config *cfg, int hours);

/*
 * Whether the last check was at least `hours` ago, or has never happened.
 * Timestamps are fixed-width UTC, so this is a string comparison.
 */
int config_update_due(config *cfg, int hours);

/* Records that a check just happened, and the version it found. */
void config_update_checked(config *cfg, const char *latest);

/* The newest version a check has seen, or NULL when none has. */
const char *config_update_latest(config *cfg);

/*
 * When the last check happened, as the fixed-width UTC stamp it is stored as,
 * or NULL when none has. config_update_due answers "is another one owed?"
 * without this; this is for showing the moment to somebody.
 */
const char *config_update_last_check(config *cfg);

/* Whether the user has already said no to this particular version. */
int config_update_declined(config *cfg, const char *version);

/* Remembers that they did, so they are asked once per version, not per day. */
void config_update_decline(config *cfg, const char *version);

/*
 * Records that an update to `version` went through, for the binary that
 * replaces this one to tell the user about. The process that did the work
 * hands over to the new one and is gone by the time there is news to give,
 * so the news is left here rather than shown.
 */
void config_update_applied(config *cfg, const char *version);

/*
 * The version an update installed and has not announced yet, or NULL. Owned
 * by the config, and only good until the next call that writes to it.
 */
const char *config_update_unannounced(config *cfg);

/* Clears that record, so the news is given once and not on every run after. */
void config_update_announced(config *cfg);

/* ---- What the front-end remembers -------------------------------------- */

/*
 * The theme the graphical front-end was last left on, as one of the two
 * CONFIG_THEME_ strings, or NULL when it has never been set. Owned by the
 * config, and only good until the next call that writes to it.
 */
const char *config_gui_theme(config *cfg);

/* Remembers the one it has just been switched to. */
void config_set_gui_theme(config *cfg, const char *theme);

/*
 * The two things the window can be told to stop doing: showing the bar along
 * its bottom, and keeping what that bar says in the logfile. Both are on when
 * the key is absent, which is how a state file that has never been written to
 * leaves them, and how they have always behaved.
 */
int config_gui_status_bar(config *cfg);
void config_set_gui_status_bar(config *cfg, int on);

int config_gui_save_logs(config *cfg);
void config_set_gui_save_logs(config *cfg, int on);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_CONFIG_H */
