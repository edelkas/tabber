/*
 * tabber - custom tab installer for N++.
 *
 * Current scope: locating the game's directories, keeping the catalogue of
 * available custom tabs (the digest) up to date, and installing a tab: its
 * level and challenge files, the palettes it bundles, the library patch that
 * redirects the game's queries, the savefile, and the game's own texts. It also
 * binds several players' controls together, which co-op tabs need.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloud.h"
#include "config.h"
#include "digest.h"
#include "install.h"
#include "keys.h"
#include "loc.h"
#include "palettes.h"
#include "patch.h"
#include "paths.h"
#include "platform.h"
#include "save.h"
#include "server.h"
#include "tabs.h"
#include "update.h"
#include "util.h"
#include "version.h"

/* Labels used by the default (human-readable) output of `paths`. */
#define LABEL_STEAM     "Steam folder:       "
#define LABEL_LIBRARY   "Steam library:      "
#define LABEL_INSTALL   "Installation dir:   "
#define LABEL_PERSONAL  "Personal dir:       "

/* Layout of the `list` table; its column headers are in digest.h. */
#define COL_GAP         2      /* spaces between columns          */
#define COL_NAME_MAX    32     /* longer values are ellipsised    */
#define COL_AUTHORS_MAX 28
#define COL_ELLIPSIS    "..."

/* Width of the step labels in the `fetch` log. */
#define FETCH_LABEL_WIDTH 9

/* Languages a log line names one by one before giving their count instead. */
#define LOG_MAX_LANGUAGES 4

/* Exit codes. */
#define EXIT_OK         0
#define EXIT_NOT_FOUND  1
#define EXIT_FAILED     2
#define EXIT_USAGE      3

typedef struct {
    int bare;          /* machine-readable output, no headers or labels */
    int verbose;       /* extra detail */
    int offline;       /* never touch the network */
    int compress;      /* gzip the savefile we hand the game, when it reads gzip */
    cloud_mode cloud;  /* what to do with Steam's copies of the savefile */
    palette_collision palettes;  /* what to do when a palette name is taken */
    int keep_palettes;           /* leave the tab's palettes behind on uninstall */
    loc_langs languages;         /* which languages of the in-game texts to write */
    int no_update_check;         /* do not look for a newer tabber this run */
} options;

/* Turns the command line into what install and uninstall take. */
static install_options install_opts(const options *opts)
{
    install_options out;

    install_options_init(&out);
    out.save_flags = opts->compress ? SAVE_FORCE_COMPRESS : 0u;
    out.cloud = opts->cloud;
    out.palettes = opts->palettes;
    out.keep_palettes = opts->keep_palettes;
    out.languages = &opts->languages;
    return out;
}

static void print_usage(FILE *out)
{
    fprintf(out,
        "Usage: " TABBER_NAME " [options] [command]\n"
        "\n"
        "Commands:\n"
        "  paths            Locate N++'s installation and personal directories (default)\n"
        "  list             List the custom tabs available in the digest\n"
        "  update           Download the latest digest of custom tabs\n"
        "  fetch CODE       Download, verify and unpack the custom tab CODE\n"
        "  remove CODE      Delete the downloaded files of the custom tab CODE\n"
        "  install CODE     Install the custom tab CODE into the game (fetching it if needed)\n"
        "  uninstall CODE   Restore the game's original files, undoing an install\n"
        "  upgrade          Update tabber itself to the newest release\n"
        "  bind LIST        Give the players in LIST (e.g. 1,2) the first one's controls\n"
        "  unbind [LIST]    Restore the controls 'bind' changed, or clear LIST's\n"
        "  check            Verify the game library matches the recorded state\n"
        "  server           Check that the 3rd party server is up\n"
        "\n"
        "Options:\n"
        "  -b, --bare       Machine-readable output: paths or tab-separated fields only\n"
        "  -v, --verbose    Print extra detail\n"
        "  -o, --offline    Skip the automatic digest refresh, use the cached copy\n"
        "      --no-update-check\n"
        "                   Do not look for a newer tabber on this run\n"
        "  -c, --force-compress\n"
        "                   Gzip the savefile put in place, when the game reads gzip\n"
        "      --cloud-mode MODE\n"
        "                   What to do with Steam Cloud's copy of the savefile:\n"
        "                   replace (default), remove, or keep it untouched\n"
        "      --on-palette-collision MODE\n"
        "                   What to do when a bundled palette's name is taken:\n"
        "                   skip (default), replace it, or suffix it with a number\n"
        "      --keep-palettes\n"
        "                   Leave the tab's palettes in the game when uninstalling\n"
        "      --languages LIST\n"
        "                   Which languages of the in-game texts a tab replaces:\n"
        "                   all (default), none, or a comma-separated list\n"
        "  -h, --help       Show this help and exit\n"
        "  -V, --version    Show the version and exit\n");
}

/* ---- paths ------------------------------------------------------------- */

static void print_path(const char *label, const char *path, const options *opts)
{
    if (opts->bare)
        printf("%s\n", path);
    else
        printf("%s%s\n", label, path);
}

static int cmd_paths(const options *opts)
{
    npp_paths paths = {0};
    char err[TB_ERR_LEN];
    int failures = 0;

    /* Installation directory, via Steam's registry entries and manifests. */
    if (npp_find_game_dirs(&paths, err, sizeof err) == 0) {
        if (opts->verbose && !opts->bare) {
            print_path(LABEL_STEAM, paths.steam_dir, opts);
            print_path(LABEL_LIBRARY, paths.library_dir, opts);
        }
        print_path(LABEL_INSTALL, paths.install_dir, opts);
    } else {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        failures++;
    }

    /* Personal directory, derived from the user's own folders. */
    if (npp_find_personal_dir(&paths, err, sizeof err) == 0) {
        print_path(LABEL_PERSONAL, paths.personal_dir, opts);
    } else {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        failures++;
    }

    npp_paths_free(&paths);
    return failures ? EXIT_NOT_FOUND : EXIT_OK;
}

/* ---- update ------------------------------------------------------------ */

static int cmd_update(const options *opts)
{
    char err[TB_ERR_LEN];
    digest *dig;

    if (opts->offline) {
        fprintf(stderr, TABBER_NAME ": --offline cannot be combined with 'update'\n");
        return EXIT_USAGE;
    }

    /* Forced: an explicit update always goes out to the network. */
    if (digest_ensure_fresh(1, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": cannot update the digest: %s\n", err);
        return EXIT_FAILED;
    }

    dig = digest_load(err, sizeof err);
    if (!dig) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_FAILED;
    }

    printf("Digest updated: %u custom tab(s)%s%s\n",
           (unsigned)dig->tab_count,
           dig->signature_date ? ", dated " : "",
           dig->signature_date ? dig->signature_date : "");
    printf("Saved to %s\n", dig->path);

    digest_free(dig);
    return EXIT_OK;
}

/* ---- list -------------------------------------------------------------- */

/*
 * Copies `text` into `out`, shortening it with an ellipsis when it is wider
 * than `max` characters. Widths are in characters, not bytes, so multi-byte
 * names stay aligned.
 */
static void fit_column(char *out, size_t outsz, const char *text, size_t max)
{
    size_t ellipsis = sizeof(COL_ELLIPSIS) - 1;
    size_t width = str_display_width(text);
    size_t kept = 0, bytes = 0;

    if (width <= max || max <= ellipsis) {
        snprintf(out, outsz, "%s", text);
        return;
    }

    /* Walk whole characters until the ellipsis still fits. */
    while (text[bytes] && kept < max - ellipsis) {
        bytes++;
        while (((unsigned char)text[bytes] & 0xC0) == 0x80)   /* stay on a character boundary */
            bytes++;
        kept++;
    }
    snprintf(out, outsz, "%.*s" COL_ELLIPSIS, (int)bytes, text);
}

/* Pads `text` on the right to `width` characters. */
static void print_cell(const char *text, size_t width, int last)
{
    size_t shown = str_display_width(text);

    fputs(text, stdout);
    if (last) {
        fputc('\n', stdout);
        return;
    }
    while (shown++ < width + COL_GAP)
        fputc(' ', stdout);
}

/* Prints a horizontal rule of `width` dashes. */
static void print_rule(size_t width, int last)
{
    size_t i;

    for (i = 0; i < width; i++)
        fputc('-', stdout);
    if (last)
        fputc('\n', stdout);
    else
        printf("%*s", COL_GAP, "");
}

static int cmd_list(const options *opts)
{
    char err[TB_ERR_LEN];
    digest *dig;
    size_t i;
    size_t w_code, w_name, w_authors, w_date;

    /* Refresh once per session, but never fail over a network hiccup: the
     * cached digest is good enough to keep working. */
    if (!opts->offline && digest_ensure_fresh(0, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": could not refresh the digest (%s), using the cached copy\n", err);

    dig = digest_load(err, sizeof err);
    if (!dig) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_FAILED;
    }

    /* Size every column to its widest value before printing anything. */
    w_code = str_display_width(COL_CODE);
    w_name = str_display_width(COL_NAME);
    w_authors = str_display_width(COL_AUTHORS);
    w_date = str_display_width(COL_DATE);
    for (i = 0; i < dig->tab_count; i++) {
        char name[256], authors[256];
        size_t width;

        fit_column(name, sizeof name, dig->tabs[i].name, COL_NAME_MAX);
        fit_column(authors, sizeof authors, dig->tabs[i].authors, COL_AUTHORS_MAX);

        width = str_display_width(dig->tabs[i].code);
        if (width > w_code) w_code = width;
        width = str_display_width(name);
        if (width > w_name) w_name = width;
        width = str_display_width(authors);
        if (width > w_authors) w_authors = width;
    }

    if (!opts->bare) {
        print_cell(COL_CODE, w_code, 0);
        print_cell(COL_NAME, w_name, 0);
        print_cell(COL_AUTHORS, w_authors, 0);
        print_cell(COL_DATE, w_date, 1);
        print_rule(w_code, 0);
        print_rule(w_name, 0);
        print_rule(w_authors, 0);
        print_rule(w_date, 1);
    }

    for (i = 0; i < dig->tab_count; i++) {
        char code[DIGEST_CODE_BUF], name[256], authors[256];
        char date[DIGEST_DATE_BUF];

        digest_code_upper(code, sizeof code, dig->tabs[i].code);
        fit_column(name, sizeof name, dig->tabs[i].name, COL_NAME_MAX);
        fit_column(authors, sizeof authors, dig->tabs[i].authors, COL_AUTHORS_MAX);
        digest_date_short(date, sizeof date, dig->tabs[i].date);

        if (opts->bare) {
            printf("%s\t%s\t%s\t%s\n", code, name, authors, date);
        } else {
            print_cell(code, w_code, 0);
            print_cell(name, w_name, 0);
            print_cell(authors, w_authors, 0);
            print_cell(date, w_date, 1);
        }
    }

    if (!opts->bare)
        printf("\n%u custom tab(s) available.\n", (unsigned)dig->tab_count);

    digest_free(dig);
    return EXIT_OK;
}

/* ---- fetch ------------------------------------------------------------- */

/* Prints one aligned "  label  detail" line of the fetch log. */
static void log_step(const char *label, const char *fmt, ...)
{
    va_list ap;

    printf("  %-*s ", FETCH_LABEL_WIDTH, label);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static int cmd_fetch(const options *opts, const char *code)
{
    char err[TB_ERR_LEN];
    digest *dig;
    const npp_tab *tab;
    tab_report report;
    char upper[16];
    int rc = EXIT_FAILED;

    if (!code) {
        fprintf(stderr, TABBER_NAME ": 'fetch' needs a tab code, e.g. '%s fetch met'\n",
                TABBER_NAME);
        return EXIT_USAGE;
    }

    if (!opts->offline && digest_ensure_fresh(0, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": could not refresh the digest (%s), using the cached copy\n", err);

    dig = digest_load(err, sizeof err);
    if (!dig) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_FAILED;
    }

    tab = digest_find(dig, code);
    if (!tab) {
        fprintf(stderr, TABBER_NAME ": no custom tab with code '%s' (run '%s list' to see them all)\n",
                code, TABBER_NAME);
        digest_free(dig);
        return EXIT_NOT_FOUND;
    }

    digest_code_upper(upper, sizeof upper, tab->code);
    printf("Fetching %s (%s)...\n", upper, tab->name);

    if (tab_fetch(dig, tab, &report, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s could not be installed: %s\n", upper, err);
        digest_free(dig);
        return EXIT_FAILED;
    }

    if (opts->verbose)
        log_step("source", "%s", report.link);
    log_step("download", "%lu bytes, MD5 %s (ok)",
             (unsigned long)report.zip_bytes, report.md5);
    log_step("archive", "%lu entries, %lu bytes uncompressed (ok)",
             (unsigned long)report.entry_count, (unsigned long)report.disk_bytes);
    log_step("contents", "%lu level file(s), %lu challenge file(s) in %s/ (ok)",
             (unsigned long)report.level_files, (unsigned long)report.challenge_files,
             digest_levels_dir(dig));
    if (report.palettes)
        log_step("palettes", "%lu bundled in %s/ (ok)",
                 (unsigned long)report.palettes, digest_palettes_dir(dig));
    log_step("extracted", "%lu file(s) to %s", (unsigned long)report.file_count, report.dir);
    if (report.state_path[0])
        log_step("recorded", "download in %s", report.state_path);
    if (report.warning[0])
        fprintf(stderr, TABBER_NAME ": warning: %s\n", report.warning);
    printf("%s fetched successfully.\n", upper);

    rc = EXIT_OK;
    tab_report_free(&report);
    digest_free(dig);
    return rc;
}

/* ---- install ----------------------------------------------------------- */

/* What became of the savefile: what was archived, and what took its place. */
static void print_save_step(const save_report *save)
{
    if (save->backed_up)
        log_step("savefile", "archived as %s (%lu bytes)",
                 save->backup_path, (unsigned long)save->backup_bytes);
    else
        log_step("savefile", "there was none to archive");
    log_step("", "%s written from %s%s", save->save_path, save->source_path,
             save->used_fresh && !save->from_builtin
                 ? " (the fresh save tabber ships)" : "");
    if (save->compressed)
        log_step("", "gzipped on the way in, %lu bytes", (unsigned long)save->save_bytes);
    if (save->removed_path[0])
        log_step("", "%s removed, so the game reads the new one", save->removed_path);
}

/* One line per palette the tab bundles, saying what became of it. */
static void print_palette_step(const palette_report *pal, palette_collision mode,
                               int installing)
{
    size_t i;

    if (pal->count == 0) {
        log_step("palettes", "the tab bundles none");
        return;
    }

    if (installing)
        log_step("palettes", "%u bundled, %u installed into %s (on collision: %s)",
                 (unsigned)pal->count, (unsigned)pal->installed, pal->dir,
                 palette_collision_name(mode));
    else if (pal->removed)
        log_step("palettes", "%u of the tab's own removed from %s",
                 (unsigned)pal->removed, pal->dir);
    else
        log_step("palettes", "%u of the tab's own left in %s",
                 (unsigned)pal->count, pal->dir);

    for (i = 0; i < pal->count; i++) {
        const palette_item *item = &pal->items[i];
        const char *as = item->target && !str_ieq(item->target, item->name)
                       ? item->target : NULL;

        if (item->outcome == PAL_FAILED) {
            fprintf(stderr, TABBER_NAME ": warning: the palette '%s' %s\n",
                    item->name, item->detail[0] ? item->detail : "could not be handled");
            continue;
        }
        if (as)
            log_step("", "'%s': %s, as '%s'", item->name,
                     palette_outcome_text(item->outcome), as);
        else if (item->detail[0])
            log_step("", "'%s': %s (%s)", item->name,
                     palette_outcome_text(item->outcome), item->detail);
        else
            log_step("", "'%s': %s", item->name, palette_outcome_text(item->outcome));
    }

    /* Worth knowing when the folder is filling up: past the line the game
     * simply stops reading, whoever the palettes belong to. */
    if (installing && pal->total > PALETTE_LIMIT)
        fprintf(stderr, TABBER_NAME ": warning: the game reads %d palettes and there "
                        "are now %u; the last ones will be ignored\n",
                PALETTE_LIMIT, (unsigned)pal->total);
}

/* "english, spanish", or their count once naming them stops helping. */
static void languages_text(char *out, size_t outsz, const str_list *names)
{
    size_t i, used = 0;

    if (names->count == 0) {
        snprintf(out, outsz, "none");
        return;
    }
    if (names->count > LOG_MAX_LANGUAGES) {
        snprintf(out, outsz, "%u languages", (unsigned)names->count);
        return;
    }
    for (i = 0; i < names->count && used + 1 < outsz; i++) {
        int written = snprintf(out + used, outsz - used, "%s%s", i ? ", " : "",
                               names->items[i]);
        if (written < 0)
            break;
        used += (size_t)written;
    }
}

/* One line per in-game text the tab changes, saying what it now reads. */
static void print_loc_step(const loc_report *loc, int installing)
{
    char langs[128];
    size_t i, touched = 0;

    if (!loc->path)
        return;              /* the plan could not be worked out; already warned */

    for (i = 0; i < loc->unknown.count; i++)
        fprintf(stderr, TABBER_NAME ": warning: the game's texts have no '%s' "
                        "language, skipped\n", loc->unknown.items[i]);

    if (loc->count == 0) {
        log_step("texts", "left alone, no language of them was selected");
        return;
    }
    for (i = 0; i < loc->count; i++)
        touched += loc->items[i].outcome == LOC_CHANGED ? 1 : 0;

    languages_text(langs, sizeof langs, &loc->languages);
    log_step("texts", "%u of %u %s in %s (%s)", (unsigned)touched, (unsigned)loc->count,
             installing ? "replaced" : "restored", loc->path, langs);

    for (i = 0; i < loc->count; i++) {
        const loc_item *item = &loc->items[i];

        if (item->outcome != LOC_CHANGED)
            log_step("", "'%s': %s", item->id, loc_outcome_text(item->outcome));
        else if (installing)
            log_step("", "'%s': \"%s\" in %u language(s)", item->id, item->text,
                     (unsigned)item->changed);
        else
            log_step("", "'%s': the original is back in %u language(s)", item->id,
                     (unsigned)item->changed);
    }
}

/* One line per binding, saying what it answers to now. */
static void print_keys_step(const keys_report *keys)
{
    size_t i;

    if (!keys->path)
        return;              /* nothing asked for them, or the plan failed */

    log_step("bindings", "%s", keys->path);
    for (i = 0; i < keys->count; i++) {
        const key_item *item = &keys->items[i];

        if (item->outcome == KEY_ABSENT)
            fprintf(stderr, TABBER_NAME ": warning: '%s' is not set in the bindings "
                            "file, so it was left alone\n", item->name);
        else if (item->outcome == KEY_SAME)
            log_step("", "%s: %s", item->name, key_outcome_text(item->outcome));
        else
            log_step("", "%s: %s -> %s", item->name, item->before, item->after);
    }
    log_step("changed", "%u binding(s)", (unsigned)keys->changed);
}

/* One line per Steam account that has N++ cloud data, whatever happened to it. */
static void print_cloud_step(const cloud_report *cloud, cloud_mode mode)
{
    size_t i;

    if (cloud->count == 0) {
        log_step("cloud", "%s", cloud->searched
                 ? "no Steam account on this machine has N++ cloud data"
                 : "Steam's folder was not found, so there is nothing to sync");
        return;
    }

    log_step("cloud", "%u account(s) with N++ data, mode '%s'",
             (unsigned)cloud->count, cloud_mode_name(mode));
    for (i = 0; i < cloud->count; i++) {
        const cloud_user *user = &cloud->users[i];

        if (user->detail[0])
            fprintf(stderr, TABBER_NAME ": warning: Steam account %s: %s\n",
                    user->id, user->detail);
        else if (user->replaced)
            log_step("", "%s: cloud save replaced, %lu bytes%s", user->id,
                     (unsigned long)user->written,
                     user->removed_raw ? " (and an outdated uncompressed one removed)" : "");
        else if (user->removed_gz)
            log_step("", "%s: cloud save removed%s", user->id,
                     user->removed_raw ? ", uncompressed one included" : "");
        else if (user->removed_raw)
            log_step("", "%s: an outdated uncompressed cloud save was removed", user->id);
        else if (user->had_gz || user->had_raw)
            log_step("", "%s: a cloud save is there and was left alone", user->id);
        else
            log_step("", "%s: no cloud save", user->id);
    }
}

/* Downloads the tab first if its files are not in the local store. */
static int ensure_downloaded(const digest *dig, const npp_tab *tab, const char *upper)
{
    char err[TB_ERR_LEN];
    tab_report report;

    if (tab_is_downloaded(tab->code))
        return 0;

    printf("%s is not downloaded yet, fetching it first...\n", upper);
    if (tab_fetch(dig, tab, &report, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s could not be downloaded: %s\n", upper, err);
        return -1;
    }
    log_step("downloaded", "%lu file(s) to %s",
             (unsigned long)report.file_count, report.dir);
    if (report.warning[0])
        fprintf(stderr, TABBER_NAME ": warning: %s\n", report.warning);
    tab_report_free(&report);
    return 0;
}

static int cmd_install(const options *opts, const char *code)
{
    char err[TB_ERR_LEN];
    char installed_code[64];
    digest *dig;
    const npp_tab *tab;
    npp_paths paths = {0};
    config *state;
    install_report report;
    char upper[16], other[16];
    size_t i;
    int rc = EXIT_FAILED;

    if (!code) {
        fprintf(stderr, TABBER_NAME ": 'install' needs a tab code, e.g. '%s install met'\n",
                TABBER_NAME);
        return EXIT_USAGE;
    }

    if (!opts->offline && digest_ensure_fresh(0, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": could not refresh the digest (%s), using the cached copy\n", err);

    dig = digest_load(err, sizeof err);
    if (!dig) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_FAILED;
    }

    tab = digest_find(dig, code);
    if (!tab) {
        fprintf(stderr, TABBER_NAME ": no custom tab with code '%s' (run '%s list' to see them all)\n",
                code, TABBER_NAME);
        digest_free(dig);
        return EXIT_NOT_FOUND;
    }
    digest_code_upper(upper, sizeof upper, tab->code);

    /* The game's own folders, installation and personal: the savefile swap
     * needs the second one, so a missing one stops the install here. */
    if (npp_find_game_dirs(&paths, err, sizeof err) != 0 ||
        npp_find_personal_dir(&paths, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }

    /* Downloading first is fine; it changes nothing in the game folder. */
    if (ensure_downloaded(dig, tab, upper) != 0)
        goto done;

    /* Only one tab may be installed at a time. */
    state = config_load(err, sizeof err);
    if (!state) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }
    if (install_detect(state, &paths, installed_code, sizeof installed_code)) {
        digest_code_upper(other, sizeof other, installed_code);
        config_free(state);
        if (!strcmp(other, upper))
            fprintf(stderr, TABBER_NAME ": %s is already installed\n", upper);
        else
            fprintf(stderr, TABBER_NAME ": %s is installed; only one custom tab can be "
                            "installed at a time, so uninstall it first\n", other);
        goto done;
    }
    config_free(state);

    printf("Installing %s (%s)...\n", upper, tab->name);

    {
        install_options run = install_opts(opts);

        if (tab_install(dig, tab, &paths, &run, &report, err, sizeof err) != 0) {
            fprintf(stderr, TABBER_NAME ": %s could not be installed: %s\n", upper, err);
            install_report_free(&report);
            goto done;
        }
    }

    /* Files the tab ships that the game has no use for. */
    for (i = 0; i < report.skipped.count; i++)
        fprintf(stderr, TABBER_NAME ": warning: '%s' is not a level or challenge file "
                        "the game reads, skipped\n", report.skipped.items[i]);

    log_step("target", "%s", report.game_levels_dir);
    log_step("installed", "%lu file(s), %lu skipped",
             (unsigned long)report.installed_count, (unsigned long)report.skipped.count);
    log_step("originals", "kept alongside with the '%s' suffix", INSTALL_BACKUP_SUFFIX);
    if (report.stale_backups.count)
        log_step("", "%lu of them replaced a leftover backup from an earlier install",
                 (unsigned long)report.stale_backups.count);
    log_step("library", "queries redirected to %s (from %s)",
             report.server_uri, report.server_source);
    if (report.credit[0])
        log_step("credit", "the game now credits '%s'", report.credit);
    if (report.health.reachable)
        log_step("server", "%s answered HTTP %d", report.health.url, report.health.status);
    else
        fprintf(stderr, TABBER_NAME ": warning: the 3rd party server does not seem to be up "
                        "(%s: %s); the tab is installed all the same, but its scores will "
                        "not work until the server is back\n",
                report.health.url, report.health.detail);
    print_palette_step(&report.palettes, opts->palettes, 1);
    print_loc_step(&report.strings, 1);
    print_keys_step(&report.bindings);
    print_save_step(&report.save);
    print_cloud_step(&report.cloud, opts->cloud);
    if (report.state_path[0])
        log_step("recorded", "install in %s", report.state_path);
    if (report.warning[0])
        fprintf(stderr, TABBER_NAME ": warning: %s\n", report.warning);
    printf("%s installed successfully.\n", upper);

    install_report_free(&report);
    rc = EXIT_OK;

done:
    npp_paths_free(&paths);
    digest_free(dig);
    return rc;
}

/* ---- check ------------------------------------------------------------- */

/* Names the library state in a way that reads well in a report. */
static const char *lib_state_text(lib_state state)
{
    switch (state) {
        case LIB_ORIGINAL: return "official server (no tab installed)";
        case LIB_PATCHED:  return "3rd party server (a tab is installed)";
        default:           return "unrecognised";
    }
}

static int cmd_check(const options *opts)
{
    char err[TB_ERR_LEN];
    digest *dig;
    config *state;
    npp_paths paths = {0};
    lib_health health;
    int rc = EXIT_FAILED;

    (void)opts;

    dig = digest_load(err, sizeof err);   /* may be absent: only used for a server key */
    state = config_load(err, sizeof err);
    if (!state) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        digest_free(dig);
        return EXIT_FAILED;
    }

    if (npp_find_game_dirs(&paths, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }

    if (lib_check(state, dig, &paths, &health, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }
    /* The verdict is part of the state, so it is saved whatever it says. */
    if (config_save(state, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": warning: the check was not recorded: %s\n", err);

    log_step("library", "%s", lib_state_text(health.state));
    log_step("points at", "%s", health.uri[0] ? health.uri : "(nothing recognisable)");
    log_step("recorded", "%s", health.state_code[0] ? health.state_code : "no tab installed");
    if (health.healthy) {
        /* Passed, but worth saying when the tab in the game is not one of
         * ours: uninstalling it will work, and may take a download first. */
        if (health.unrecorded)
            log_step("note", "%s", health.detail);
        printf("Library check passed.\n");
        rc = EXIT_OK;
    } else {
        fprintf(stderr, TABBER_NAME ": library check FAILED: %s\n", health.detail);
        rc = EXIT_FAILED;
    }

done:
    npp_paths_free(&paths);
    config_free(state);
    digest_free(dig);
    return rc;
}

/* ---- server ------------------------------------------------------------ */

/*
 * Asks the 3rd party server whether it is listening. The endpoint does not
 * exist yet, so a 404 is the expected answer and counts as a pass: it still
 * proves something is answering on that host and port.
 */
static int cmd_server(const options *opts)
{
    char err[TB_ERR_LEN];
    digest *dig;
    config *state;
    server_health health;
    int up;

    if (opts->offline) {
        fprintf(stderr, TABBER_NAME ": --offline cannot be combined with 'server'\n");
        return EXIT_USAGE;
    }

    /* Both files only matter here for a "server" key; neither has to be there. */
    dig = digest_load(err, sizeof err);
    state = config_load(err, sizeof err);
    if (!state) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        digest_free(dig);
        return EXIT_FAILED;
    }

    up = server_check(state, dig, &health);
    config_free(state);
    digest_free(dig);

    if (opts->bare) {
        printf("%s\t%d\t%s\n", up ? "up" : "down", health.status, health.url);
        return up ? EXIT_OK : EXIT_FAILED;
    }

    log_step("address", "%s (from %s)", health.url, server_source_name(health.source));
    if (up) {
        log_step("reply", "HTTP %d", health.status);
        printf("Server check passed: %s is listening.\n", health.addr.host);
        return EXIT_OK;
    }

    fprintf(stderr, TABBER_NAME ": server check FAILED: %s\n", health.detail);
    return EXIT_FAILED;
}

/* ---- uninstall --------------------------------------------------------- */

static int cmd_uninstall(const options *opts, const char *code)
{
    char err[TB_ERR_LEN];
    digest *dig;
    const npp_tab *tab;
    npp_paths paths = {0};
    uninstall_report report;
    char upper[16];
    size_t i;
    int rc = EXIT_FAILED;

    if (!code) {
        fprintf(stderr, TABBER_NAME ": 'uninstall' needs a tab code, e.g. '%s uninstall met'\n",
                TABBER_NAME);
        return EXIT_USAGE;
    }

    /* Uninstalling is a local repair job: use whatever digest is at hand. */
    dig = digest_load(err, sizeof err);
    if (!dig) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_FAILED;
    }

    tab = digest_find(dig, code);
    if (!tab) {
        fprintf(stderr, TABBER_NAME ": no custom tab with code '%s' (run '%s list' to see them all)\n",
                code, TABBER_NAME);
        digest_free(dig);
        return EXIT_NOT_FOUND;
    }
    digest_code_upper(upper, sizeof upper, tab->code);

    /* Both folders again: the savefile has to be swapped back too. */
    if (npp_find_game_dirs(&paths, err, sizeof err) != 0 ||
        npp_find_personal_dir(&paths, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }

    printf("Uninstalling %s (%s)...\n", upper, tab->name);

    /* The files on disk decide, not the state file: a config that drifted out
     * of step must not stop a real installation from being undone. */
    {
        install_options run = install_opts(opts);

        if (tab_uninstall(dig, tab, &paths, &run, &report, err, sizeof err) != 0) {
            fprintf(stderr, TABBER_NAME ": %s could not be uninstalled: %s\n", upper, err);
            uninstall_report_free(&report);
            goto done;
        }
    }

    for (i = 0; i < report.skipped.count; i++)
        fprintf(stderr, TABBER_NAME ": warning: '%s' is not a level or challenge file "
                        "the game reads, skipped\n", report.skipped.items[i]);
    for (i = 0; i < report.leftovers.count; i++)
        fprintf(stderr, TABBER_NAME ": warning: '%s%s' is still in the game folder and "
                        "was not part of this tab\n", report.leftovers.items[i],
                INSTALL_BACKUP_SUFFIX);

    log_step("target", "%s", report.game_levels_dir);
    log_step("restored", "%lu original file(s)", (unsigned long)report.restored_count);
    if (report.from_originals) {
        /* The rest of them had no backup, which is what an install by one of
         * the older installers leaves behind. */
        char originals[TAB_CODE_MAX_LEN + 1];

        digest_code_upper(originals, sizeof originals, report.originals_code);
        log_step("", "%lu from an '%s' backup, %lu from the %s tab%s",
                 (unsigned long)report.from_backups, INSTALL_BACKUP_SUFFIX,
                 (unsigned long)report.from_originals, originals,
                 report.fetched_originals ? ", downloaded just now" : "");
    }
    log_step("library", "queries point back at %s", report.server_uri);
    if (report.credit_restored)
        log_step("credit", "the game credits '%s' again", LIB_CREDIT_ORIGINAL);
    print_palette_step(&report.palettes, opts->palettes, 0);
    print_loc_step(&report.strings, 0);
    print_keys_step(&report.bindings);
    print_save_step(&report.save);
    print_cloud_step(&report.cloud, opts->cloud);
    if (report.state_path[0])
        log_step("recorded", "uninstall in %s", report.state_path);
    if (report.warning[0])
        fprintf(stderr, TABBER_NAME ": warning: %s\n", report.warning);
    printf("%s uninstalled successfully.\n", upper);

    uninstall_report_free(&report);
    rc = EXIT_OK;

done:
    npp_paths_free(&paths);
    digest_free(dig);
    return rc;
}

/* ---- bind / unbind ----------------------------------------------------- */

/* Reads a player list, or explains why it is not one. */
static int read_players(const char *list, int *players, size_t *count)
{
    char err[TB_ERR_LEN];

    if (keys_players_parse(list, players, count, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return -1;
    }
    return 0;
}

/* "2 and 3", or "2, 3 and 4": the players about to answer to another's keys. */
static char *players_text(const int *players, size_t from, size_t count)
{
    byte_buf text = {0};
    size_t i;

    for (i = from; i < count; i++) {
        char one[16];

        if (i > from)
            buf_append(&text, i + 1 == count ? " and " : ", ", i + 1 == count ? 5 : 2);
        snprintf(one, sizeof one, "%d", players[i]);
        buf_append(&text, one, strlen(one));
    }
    return buf_finish(&text, NULL);
}

static int cmd_bind(const options *opts, const char *list)
{
    char err[TB_ERR_LEN];
    npp_paths paths = {0};
    config *state = NULL;
    keys_plan plan;
    keys_report report;
    int players[KEYS_PLAYER_MAX];
    size_t count = 0, recorded, i;
    char *names;
    int planned = 0, rc = EXIT_FAILED;

    (void)opts;

    if (!list) {
        fprintf(stderr, TABBER_NAME ": 'bind' needs a list of players, e.g. "
                        "'%s bind 1,2'\n", TABBER_NAME);
        return EXIT_USAGE;
    }
    if (read_players(list, players, &count) != 0)
        return EXIT_USAGE;
    if (count < 2) {
        fprintf(stderr, TABBER_NAME ": 'bind' needs at least two players: the one whose "
                        "keys are copied, and one to copy them to\n");
        return EXIT_USAGE;
    }

    if (npp_find_personal_dir(&paths, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_NOT_FOUND;
    }
    state = config_load(err, sizeof err);
    if (!state) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }

    if (keys_bind_build(&paths, players, count, config_get_keybindings(state),
                        &plan, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the controls could not be bound: %s\n", err);
        goto done;
    }
    planned = 1;

    names = players_text(players, 1, count);
    printf("Binding player%s %s to player %d's controls...\n",
           count > 2 ? "s" : "", names, players[0]);
    free(names);

    if (keys_plan_apply(&plan, &report, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the controls could not be bound: %s\n", err);
        keys_report_free(&report);
        goto done;
    }
    print_keys_step(&report);

    for (i = 0; i < report.count; i++) {
        if (report.items[i].outcome == KEY_BOUND &&
            !strcmp(report.items[i].after, KEYS_UNBOUND)) {
            fprintf(stderr, TABBER_NAME ": warning: player %d has no key bound to some "
                            "of its controls, so neither do the players copying it\n",
                    report.source);
            break;
        }
    }
    keys_report_free(&report);

    /* The originals, so `unbind` can put them back. Without that record the
     * change would not be undoable, which is reason enough to take it back. */
    recorded = json_count(plan.record);
    config_set_keybindings(state, keys_plan_take_record(&plan));
    if (config_save(state, err, sizeof err) != 0) {
        keys_plan_undo(&plan);
        fprintf(stderr, TABBER_NAME ": the original bindings could not be recorded "
                        "(%s), so the controls were left as they were\n", err);
        goto done;
    }
    log_step("recorded", "%u original binding(s) in %s", (unsigned)recorded, state->path);
    printf("Controls bound.\n");
    rc = EXIT_OK;

done:
    if (planned)
        keys_plan_free(&plan);
    if (state)
        config_free(state);
    npp_paths_free(&paths);
    return rc;
}

static int cmd_unbind(const options *opts, const char *list)
{
    char err[TB_ERR_LEN];
    npp_paths paths = {0};
    config *state = NULL;
    keys_plan plan;
    keys_report report;
    int players[KEYS_PLAYER_MAX];
    size_t count = 0;
    int planned = 0, rc = EXIT_FAILED;

    (void)opts;

    /* The list is optional: without it, only what is on record is put back. */
    if (list && read_players(list, players, &count) != 0)
        return EXIT_USAGE;

    if (npp_find_personal_dir(&paths, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        return EXIT_NOT_FOUND;
    }
    state = config_load(err, sizeof err);
    if (!state) {
        fprintf(stderr, TABBER_NAME ": %s\n", err);
        goto done;
    }

    if (keys_unbind_build(&paths, players, count, config_get_keybindings(state),
                          &plan, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the controls could not be restored: %s\n", err);
        goto done;
    }
    planned = 1;

    if (plan.count == 0) {
        printf("No controls are on record as changed, and no player was named, so "
               "there is nothing to undo.\n");
        rc = EXIT_OK;
        goto done;
    }

    printf("Restoring the controls tabber changed...\n");
    if (keys_plan_apply(&plan, &report, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the controls could not be restored: %s\n", err);
        keys_report_free(&report);
        goto done;
    }
    print_keys_step(&report);
    keys_report_free(&report);

    /* Nothing is changed any more, so nothing is on record any more. */
    config_set_keybindings(state, keys_plan_take_record(&plan));
    if (config_save(state, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": warning: the controls were restored but the "
                        "record of them was not cleared: %s\n", err);
    else
        log_step("recorded", "the record of changed controls is empty again");
    printf("Controls restored.\n");
    rc = EXIT_OK;

done:
    if (planned)
        keys_plan_free(&plan);
    if (state)
        config_free(state);
    npp_paths_free(&paths);
    return rc;
}

/* ---- upgrade ----------------------------------------------------------- */

/* Asks a yes/no question, with yes as the answer for someone who just hits
 * return. No answer at all — a closed input — is not a yes. */
static int confirm(const char *question)
{
    char line[32];

    printf("%s [Y/n] ", question);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin))
        return 0;
    return line[0] == '\n' || line[0] == 'y' || line[0] == 'Y';
}

/* What a release says about itself, in the shape of the other reports. */
static void print_release(const update_info *info)
{
    log_step("installed", "%s (%s, %s)", TABBER_VERSION, PLAT_NAME, UPDATE_BUILD_KEY);
    log_step("latest", "%s%s%s", info->version,
             info->date[0] ? ", released " : "", info->date);
    if (info->notes && info->notes[0])
        log_step("notes", "%s", info->notes);
    if (info->page && info->page[0])
        log_step("page", "%s", info->page);
}

/*
 * Downloads a release and puts it in place of the running binary. Returns 0
 * when tabber has been replaced, -1 when nothing changed. `exe_out`, when
 * given, receives the path the new binary now answers to — which is not
 * something to go asking the system for afterwards: on Linux a process's own
 * path follows the file it was started from, and that file has just been
 * renamed out of the way.
 */
static int apply_upgrade(const update_info *info, char **exe_out)
{
    char err[TB_ERR_LEN];
    update_plan plan;

    if (exe_out)
        *exe_out = NULL;

    printf("Downloading " TABBER_NAME " %s...\n", info->version);
    if (update_plan_build(info, &plan, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the update was not applied: %s\n", err);
        return -1;
    }
    log_step("download", "%lu bytes, MD5 %s (ok)", (unsigned long)plan.bytes, info->md5);

    if (update_plan_apply(&plan, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the update was not applied: %s\n", err);
        update_plan_free(&plan);
        return -1;
    }
    log_step("replaced", "%s", plan.exe);
    log_step("checked", "the new binary runs and reports %s", plan.version);
    log_step("previous", "kept as %s until the next run", plan.aside);

    if (exe_out)
        *exe_out = str_dup(plan.exe);
    update_plan_free(&plan);
    printf(TABBER_NAME " %s is in place.\n", info->version);
    return 0;
}

/* Remembers that a check just happened, so the next run does not repeat it. */
static void record_check(const update_info *info, const char *declined)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);

    if (!cfg)
        return;
    config_update_checked(cfg, info->version);
    if (declined)
        config_update_decline(cfg, declined);
    config_save(cfg, err, sizeof err);
    config_free(cfg);
}

static int cmd_upgrade(const options *opts)
{
    char err[TB_ERR_LEN];
    update_info info;
    int rc;

    if (opts->offline) {
        fprintf(stderr, TABBER_NAME ": --offline cannot be combined with 'upgrade'\n");
        return EXIT_USAGE;
    }

    printf("Checking for a newer " TABBER_NAME "...\n");
    if (update_check(&info, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": the newest release could not be looked up: %s\n"
                        "Releases are listed at %s\n", err, UPDATE_RELEASES_URL);
        return EXIT_FAILED;
    }
    print_release(&info);
    record_check(&info, NULL);

    if (!info.newer) {
        printf("You already have the newest release.\n");
        rc = EXIT_OK;
    } else if (!info.url) {
        fprintf(stderr, TABBER_NAME ": that release ships no build for %s, so it cannot "
                        "be installed from here; see %s\n", UPDATE_BUILD_KEY, info.page);
        rc = EXIT_FAILED;
    } else {
        /* Asked for by name, so it goes ahead without asking again. */
        rc = apply_upgrade(&info, NULL) == 0 ? EXIT_OK : EXIT_FAILED;
    }

    update_info_free(&info);
    return rc;
}

/*
 * The check that runs before an ordinary command: at most once a day, never
 * when the output is being piped somewhere, and never fatal. Returns 1 when
 * tabber replaced itself and handed the original command to the new binary,
 * in which case *status is what to exit with.
 */
static int check_for_update(const options *opts, const char *command,
                            int argc, char **argv, int *status)
{
    char err[TB_ERR_LEN];
    config *cfg;
    update_info info;
    char *guard, *exe = NULL;
    int upgraded = 0, restarted = 0;

    if (opts->offline || opts->no_update_check || opts->bare)
        return 0;
    if (command && !strcmp(command, "upgrade"))
        return 0;                    /* that command does this properly */

    /* The restarted process must not go looking all over again. */
    guard = plat_getenv(UPDATE_ENV_GUARD);
    if (guard) {
        free(guard);
        return 0;
    }

    /* A state file that will not load is a problem for the command itself to
     * report, not for a courtesy check to fail on. */
    cfg = config_load(err, sizeof err);
    if (!cfg)
        return 0;
    if (!config_update_enabled(cfg) || !config_update_due(cfg, UPDATE_CHECK_HOURS)) {
        config_free(cfg);
        return 0;
    }

    if (update_check(&info, err, sizeof err) != 0) {
        /*
         * No network, or GitHub having a bad day. Not this command's problem —
         * but the attempt is still recorded, or a machine that cannot reach
         * GitHub at all would pay for a failed lookup on every command it runs
         * instead of on one a day.
         */
        if (opts->verbose)
            fprintf(stderr, TABBER_NAME ": the check for a newer version did not go "
                            "through: %s\n", err);
        config_update_checked(cfg, NULL);
        config_save(cfg, err, sizeof err);
        config_free(cfg);
        return 0;
    }
    config_update_checked(cfg, info.version);

    if (info.newer && !config_update_declined(cfg, info.version)) {
        fprintf(stderr, "\n" TABBER_NAME " %s is available; you have "
                        TABBER_VERSION ".\n", info.version);
        if (info.notes && info.notes[0])
            fprintf(stderr, "%s\n", info.notes);

        if (!info.url) {
            fprintf(stderr, "It ships no build for %s; see %s\n\n",
                    UPDATE_BUILD_KEY, info.page);
        } else if (!plat_is_interactive()) {
            /* Piped or scripted: say it once and get out of the way. */
            fprintf(stderr, "Run '" TABBER_NAME " upgrade' to install it.\n\n");
        } else if (!confirm("Update now?")) {
            config_update_decline(cfg, info.version);   /* once per version */
        } else {
            upgraded = apply_upgrade(&info, &exe) == 0;
        }
    }

    config_save(cfg, err, sizeof err);
    config_free(cfg);

    /*
     * The command the user actually typed still has to run, and now there is a
     * newer tabber to run it. If the restart cannot be made, this process
     * carries on with the code it was already running, which is no worse than
     * not having updated at all.
     */
    if (upgraded) {
        printf("Restarting...\n\n");
        plat_setenv(UPDATE_ENV_GUARD, "1");
        if (exe && plat_restart(exe, argv + 1, (size_t)(argc - 1), status) == 0)
            restarted = 1;
        else
            fprintf(stderr, TABBER_NAME ": warning: the new version could not be "
                            "started; carrying on with this one\n");
    }
    free(exe);

    update_info_free(&info);
    return restarted;
}

/* ---- remove ------------------------------------------------------------ */

static int cmd_remove(const options *opts, const char *code)
{
    char err[TB_ERR_LEN];
    digest *dig;
    const npp_tab *tab;
    tab_remove_report report;
    char upper[16];
    const char *name = NULL;
    int id = -1;

    (void)opts;

    if (!code) {
        fprintf(stderr, TABBER_NAME ": 'remove' needs a tab code, e.g. '%s remove met'\n",
                TABBER_NAME);
        return EXIT_USAGE;
    }

    /* Removal is purely local: the digest is only consulted, never refreshed,
     * and its absence is not a reason to refuse. */
    dig = digest_load(err, sizeof err);
    tab = dig ? digest_find(dig, code) : NULL;
    if (tab) {
        id = tab->id;
        code = tab->code;
        name = tab->name;
    }

    digest_code_upper(upper, sizeof upper, code);
    if (tab_remove(code, id, &report, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s could not be removed: %s\n", upper, err);
        digest_free(dig);
        return EXIT_FAILED;
    }

    if (!report.had_files && !report.recorded) {
        fprintf(stderr, TABBER_NAME ": %s is not downloaded; nothing to remove\n", upper);
        tab_remove_report_free(&report);
        digest_free(dig);
        return EXIT_NOT_FOUND;
    }

    if (name)
        printf("Removing %s (%s)...\n", upper, name);
    else
        printf("Removing %s...\n", upper);

    if (report.had_files)
        log_step("deleted", "%s", report.dir);
    else
        log_step("deleted", "nothing on disk, %s was already gone", report.dir);
    if (report.recorded)
        log_step("recorded", "removal in %s", report.state_path);
    if (report.warning[0])
        fprintf(stderr, TABBER_NAME ": warning: %s\n", report.warning);
    printf("%s removed successfully.\n", upper);

    tab_remove_report_free(&report);
    digest_free(dig);
    return EXIT_OK;
}

/* ---- Entry point ------------------------------------------------------- */

/* Runs the command the arguments named, or says there is no such thing. */
static int run_command(const options *opts, const char *command, const char *argument)
{
    if (!command || !strcmp(command, "paths"))
        return cmd_paths(opts);
    if (!strcmp(command, "list"))
        return cmd_list(opts);
    if (!strcmp(command, "update"))
        return cmd_update(opts);
    if (!strcmp(command, "fetch"))
        return cmd_fetch(opts, argument);
    if (!strcmp(command, "remove"))
        return cmd_remove(opts, argument);
    if (!strcmp(command, "install"))
        return cmd_install(opts, argument);
    if (!strcmp(command, "uninstall"))
        return cmd_uninstall(opts, argument);
    if (!strcmp(command, "upgrade"))
        return cmd_upgrade(opts);
    if (!strcmp(command, "bind"))
        return cmd_bind(opts, argument);
    if (!strcmp(command, "unbind"))
        return cmd_unbind(opts, argument);
    if (!strcmp(command, "check"))
        return cmd_check(opts);
    if (!strcmp(command, "server"))
        return cmd_server(opts);

    fprintf(stderr, TABBER_NAME ": unknown command '%s'\n", command);
    print_usage(stderr);
    return EXIT_USAGE;
}

/*
 * Up to 0.1.1 the tool kept its files beside the executable. Anything still
 * there is moved to the root used now, once, so upgrading does not lose the
 * record of which tab is installed - the one thing an uninstall cannot work
 * out for itself. Nothing already in the new root is written over, and an
 * explicit TABBER_HOME is left to whoever set it.
 */
static void adopt_old_root(void)
{
    static const char *const state[] = {
        CONFIG_FILENAME, DIGEST_FILENAME, TABS_DIR_NAME
    };
    char *home, *root, *exedir, *root_real, *exe_real;
    size_t moved = 0;

    home = plat_getenv(TABBER_ENV_HOME);
    if (home) {
        free(home);
        return;
    }

    root = plat_app_root();
    exedir = plat_exe_dir();
    if (!root || !exedir) {
        free(root);
        free(exedir);
        return;
    }

    /* Where the two are the same place there is nothing to move, and the
     * spelling on disk is what settles that. */
    root_real = plat_canonical_path(root);
    exe_real = plat_canonical_path(exedir);
    if (root_real && exe_real && strcmp(root_real, exe_real) != 0)
        plat_move_entries(exedir, root, state,
                          sizeof state / sizeof *state, &moved);

    if (moved)
        fprintf(stderr, TABBER_NAME ": moved %u item(s) that used to sit beside "
                "the executable into %s\n", (unsigned)moved, root);

    free(root);
    free(exedir);
    free(root_real);
    free(exe_real);
}

int main(int argc, char **argv)
{
    options opts = {0};
    const char *command = NULL;
    const char *argument = NULL;
    int i, rc;

    plat_init();

    /* A binary an earlier update moved aside can only be deleted once the run
     * that was using it has ended, which is to say now. */
    update_sweep();
    adopt_old_root();

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_usage(stdout);
            return EXIT_OK;
        } else if (!strcmp(arg, "-V") || !strcmp(arg, "--version")) {
            /* The architecture too: Windows ships an x64 and an x86 build. */
            printf(TABBER_NAME " " TABBER_VERSION " (" PLAT_NAME " "
                   UPDATE_ARCH ")\n");
            return EXIT_OK;
        } else if (!strcmp(arg, "-b") || !strcmp(arg, "--bare")) {
            opts.bare = 1;
        } else if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
            opts.verbose = 1;
        } else if (!strcmp(arg, "-o") || !strcmp(arg, "--offline")) {
            opts.offline = 1;
        } else if (!strcmp(arg, "--no-update-check")) {
            opts.no_update_check = 1;
        } else if (!strcmp(arg, UPDATE_SELF_CHECK_ARG)) {
            /*
             * How a freshly installed binary proves it is what the manifest
             * said it was: the old one runs this on the new one and keeps it
             * only if it agrees. Undocumented on purpose — it is not a thing
             * to type, it is a thing tabber asks itself.
             */
            const char *want = i + 1 < argc ? argv[++i] : "";

            return strcmp(want, TABBER_VERSION) == 0 ? EXIT_OK : EXIT_FAILED;
        } else if (!strcmp(arg, "-c") || !strcmp(arg, "--force-compress")) {
            opts.compress = 1;
        } else if (!strncmp(arg, "--cloud-mode", 12)) {
            /* Either "--cloud-mode=MODE" or "--cloud-mode MODE". */
            const char *value = arg[12] == '=' ? arg + 13
                              : arg[12] == '\0' && i + 1 < argc ? argv[++i] : NULL;

            if (cloud_mode_parse(value, &opts.cloud) != 0) {
                fprintf(stderr, TABBER_NAME ": '%s' is not a cloud mode; use replace, "
                                "remove or keep\n", value ? value : "");
                return EXIT_USAGE;
            }
        } else if (!strncmp(arg, "--languages", 11)) {
            /* Either "--languages=LIST" or the list as a word. */
            const char *value = arg[11] == '=' ? arg + 12
                              : arg[11] == '\0' && i + 1 < argc ? argv[++i] : NULL;

            loc_langs_free(&opts.languages);
            if (loc_langs_parse(value, &opts.languages) != 0) {
                fprintf(stderr, TABBER_NAME ": '%s' names no language; use all, none "
                                "or a comma-separated list\n", value ? value : "");
                return EXIT_USAGE;
            }
        } else if (!strcmp(arg, "--keep-palettes")) {
            opts.keep_palettes = 1;
        } else if (!strncmp(arg, "--on-palette-collision", 22)) {
            /* Either "--on-palette-collision=MODE" or the mode as a word. */
            const char *value = arg[22] == '=' ? arg + 23
                              : arg[22] == '\0' && i + 1 < argc ? argv[++i] : NULL;

            if (palette_collision_parse(value, &opts.palettes) != 0) {
                fprintf(stderr, TABBER_NAME ": '%s' is not a palette collision mode; "
                                "use skip, replace or suffix\n", value ? value : "");
                return EXIT_USAGE;
            }
        } else if (arg[0] == '-') {
            fprintf(stderr, TABBER_NAME ": unknown option '%s'\n", arg);
            print_usage(stderr);
            return EXIT_USAGE;
        } else if (!command) {
            command = arg;         /* first bare word is the command  */
        } else if (!argument) {
            argument = arg;        /* second one is its argument      */
        } else {
            fprintf(stderr, TABBER_NAME ": unexpected argument '%s'\n", arg);
            return EXIT_USAGE;
        }
    }

    /* Before the command, so that saying yes to an update means the command
     * runs on the new version rather than the one being replaced. */
    if (check_for_update(&opts, command, argc, argv, &rc)) {
        loc_langs_free(&opts.languages);
        return rc;                   /* the new binary ran it; pass on its verdict */
    }

    rc = run_command(&opts, command, argument);
    loc_langs_free(&opts.languages);
    return rc;
}
