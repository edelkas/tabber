/*
 * install.h - Installing a custom tab into the game.
 *
 * For now this covers the level and challenge files: the tab's own copies are
 * written into the game's levels folder, and each original is kept beside it
 * with an "OG" suffix appended to the whole file name (SI.txt -> SI.txtOG).
 * The game ignores those, and uninstalling is then a matter of deleting the
 * tab's files and renaming the originals back.
 *
 * The installers that came before tabber did it differently: they kept no
 * copy of anything, and put the game back by writing out the originals they
 * shipped themselves. So an "OG" file proves nothing in either direction — a
 * tab installed by one of those has none, and one uninstalled by one of those
 * leaves ours behind — and neither an install nor an uninstall may read
 * anything into their presence or absence:
 *
 *   - Installing overwrites a backup that is already there. By then the
 *     library check has established that no tab is installed, which makes any
 *     "OG" file a leftover rather than somebody's only original.
 *   - Uninstalling restores from the backups when they are there, and falls
 *     back to the game's own files for those that are not. Those come from the
 *     first mappack in the digest (INSTALL_ORIGINALS_CODE), which is the
 *     vanilla game, downloaded on the spot if it is not in the store already.
 *     Only when that cannot be had either is an uninstall refused.
 *
 * Only one custom tab can be installed at a time, so an install is refused
 * while another one is in place (see install_detect); that verdict comes from
 * the state file and the library, never from the backups. A library patched
 * for a tab the state file has never heard of is one of those installs, not a
 * fault: the library check says so (see lib_health.unrecorded) and an
 * uninstall goes ahead on the strength of it.
 *
 * Every check runs before the first rename, and any failure mid-way is rolled
 * back, so an aborted install leaves the game folder as it was.
 *
 * The palettes a tab bundles are copied in too (see palettes.h), and taken out
 * again when it is uninstalled, and a handful of the game's own texts are
 * replaced (see loc.h) and put back. A tab whose digest entry asks for it also
 * has several players' controls bound to one set of keys (see keys.h), which
 * uninstalling undoes.
 *
 * The library's developer credit is replaced with the tab's author as well,
 * the way the older installers did it, and put back on the way out.
 *
 * The savefile is swapped too (see save.h): the one in place is archived and
 * the tab's own — or the fresh one tabber ships — is put in its stead, and the
 * copies Steam Cloud keeps of it are dealt with afterwards (see cloud.h).
 *
 * Installing also asks the 3rd party server whether it is up, just before the
 * library is patched. That one is diagnostic only: the verdict is reported in
 * the install report, and a server that does not answer does not stop anything.
 */
#ifndef TABBER_INSTALL_H
#define TABBER_INSTALL_H

#include <stddef.h>

#include "cloud.h"
#include "config.h"
#include "digest.h"
#include "keys.h"
#include "loc.h"
#include "palettes.h"
#include "paths.h"
#include "save.h"
#include "server.h"
#include "util.h"

/* Appended to an original game file while a tab is installed. */
#define INSTALL_BACKUP_SUFFIX   "OG"

/*
 * The mappack whose files are the game's own: the first one in the digest.
 * Where an original comes from when no backup of it was kept.
 */
#define INSTALL_ORIGINALS_CODE  "met"

/* Written and deleted to confirm the game folder accepts changes. */
#define INSTALL_PROBE_FILE      ".tabber-write-test"

/* How many file names an error message lists before summarising. */
#define INSTALL_MAX_REPORTED    8

/* How an install or uninstall should treat the savefile. */
typedef struct {
    unsigned save_flags;          /* save.h flags: SAVE_FORCE_COMPRESS today  */
    cloud_mode cloud;             /* what to do with the Steam Cloud copies   */
    palette_collision palettes;   /* what to do when a palette name is taken  */
    int keep_palettes;            /* leave the tab's palettes behind on undo  */
    const loc_langs *languages;   /* in-game texts: NULL is every language    */
} install_options;

/* The defaults, for callers that have no opinion. */
void install_options_init(install_options *opts);

typedef struct {
    char *game_levels_dir;      /* where the files went                    */
    char *tab_levels_dir;       /* where they came from                    */
    size_t installed_count;     /* files replaced                          */
    str_list skipped;           /* tab files the game does not support     */
    str_list stale_backups;     /* 'OG' files that were already there      */
    char credit[64];            /* the title screen's new credit, if changed */
    char server_uri[128];       /* what the library now points at          */
    char server_source[32];     /* where that address came from            */
    server_health health;       /* whether that server answered            */
    palette_report palettes;    /* what happened to the bundled palettes   */
    loc_report strings;         /* ...and to the game's own texts          */
    keys_report bindings;       /* ...and to the player controls           */
    save_report save;           /* what happened to the savefile           */
    cloud_report cloud;         /* ...and to its copies in the cloud        */
    char state_path[512];       /* state file updated, empty if none       */
    char warning[TB_ERR_LEN];   /* non-fatal problem, empty if none        */
} install_report;

/*
 * Decides whether a custom tab is currently installed, copying its code into
 * `code_out`. Today the answer comes from the state file alone; `paths` is
 * taken so later versions can also weigh evidence from the game folder
 * (leftover backups, a patched library) without changing any caller.
 * Returns 1 when a tab is installed, 0 when the game is untouched.
 */
int install_detect(config *cfg, const npp_paths *paths, char *code_out, size_t code_sz);

/*
 * Installs a downloaded tab's level and challenge files. Returns 0 on success
 * and fills `report`, or -1 with a reason in `err` and the game folder
 * untouched.
 */
int tab_install(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                const install_options *opts, install_report *report,
                char *err, size_t errsz);

void install_report_free(install_report *report);

typedef struct {
    char *game_levels_dir;      /* folder that was restored                */
    size_t restored_count;      /* originals put back                      */
    size_t from_backups;        /* ...of which came from an 'OG' file      */
    size_t from_originals;      /* ...and from the vanilla mappack         */
    int fetched_originals;      /* which had to be downloaded first        */
    char originals_code[16];    /* the mappack they came from, empty if none */
    int credit_restored;        /* the title screen says Metanet again      */
    str_list skipped;           /* shipped files the game does not support */
    str_list leftovers;         /* other backups still in the game folder  */
    char server_uri[128];       /* the URI the library points at again     */
    palette_report palettes;    /* what happened to the bundled palettes   */
    loc_report strings;         /* ...and to the game's own texts          */
    keys_report bindings;       /* ...and to the player controls           */
    save_report save;           /* what happened to the savefile           */
    cloud_report cloud;         /* ...and to its copies in the cloud        */
    char state_path[512];       /* state file updated, empty if none       */
    char warning[TB_ERR_LEN];   /* non-fatal problem, empty if none        */
} uninstall_report;

/*
 * Puts the game back as it was: deletes the tab's files and renames each
 * original back over them. The file list comes from the digest rather than the
 * tab store, so uninstalling works even after the tab's download was removed.
 * Returns 0 on success, or -1 with a reason in `err` and nothing changed.
 */
int tab_uninstall(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                  const install_options *opts, uninstall_report *report,
                  char *err, size_t errsz);

void uninstall_report_free(uninstall_report *report);

#endif /* TABBER_INSTALL_H */
