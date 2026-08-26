/*
 * usage.h - When each custom tab was last played.
 *
 * Nothing records this: the game does not tell us, and tabber only ever sees
 * the moments a tab went in or came out. What it can read is the savefile,
 * which the game rewrites every time it is played, so its timestamp is the
 * closest thing to an answer there is.
 *
 * Which file that is depends on where the tab stands:
 *
 *   installed    the live savefile, nprofile.gz or nprofile. It belongs to
 *                this tab for as long as the tab is in place.
 *   uninstalled  the uninstall date in config.json, which is when its save
 *                stopped being the live one. Failing that — a tab that one of
 *                the older installers handled, so tabber never wrote it down —
 *                the archive that install left, nprofile_<code>.zip.
 *
 * A tab with none of those has never been played, which is a real answer and
 * not a missing one.
 */
#ifndef TABBER_USAGE_H
#define TABBER_USAGE_H

#include <stddef.h>

#include "config.h"
#include "paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the answer came from, so a caller can say how sure it is. */
typedef enum {
    USAGE_NEVER,          /* no sign the tab was ever played        */
    USAGE_LIVE_SAVE,      /* the live savefile: it is installed now */
    USAGE_ARCHIVED_SAVE,  /* the save its uninstall archived        */
    USAGE_RECORDED        /* a date out of the state file           */
} usage_source;

typedef struct {
    long long when;       /* Unix timestamp, 0 when never  */
    usage_source source;
} tab_usage;

/*
 * When `code` was last played, as far as can be told. `installed` says whether
 * it is the tab in place right now, which decides which file is asked. Both
 * `cfg` and `paths` may be NULL, or have nothing useful in them; the answer is
 * then whatever the other one yields, and USAGE_NEVER when neither does.
 */
void usage_last_played(config *cfg, const npp_paths *paths, const char *code,
                       int installed, tab_usage *out);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_USAGE_H */
