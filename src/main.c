/*
 * tabber - custom tab installer for N++.
 *
 * Current scope: locate the game's directories, and keep the catalogue of
 * available custom tabs (the digest) up to date. Later stages (level/challenge
 * swapping, library patching, palettes, savefile, texts) build on both.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "digest.h"
#include "install.h"
#include "paths.h"
#include "platform.h"
#include "tabs.h"
#include "util.h"
#include "version.h"

/* Labels used by the default (human-readable) output of `paths`. */
#define LABEL_STEAM     "Steam folder:       "
#define LABEL_LIBRARY   "Steam library:      "
#define LABEL_INSTALL   "Installation dir:   "
#define LABEL_PERSONAL  "Personal dir:       "

/* Column headers and layout of the `list` table. */
#define COL_CODE        "CODE"
#define COL_NAME        "NAME"
#define COL_AUTHORS     "AUTHOR(S)"
#define COL_DATE        "RELEASED"
#define COL_GAP         2      /* spaces between columns          */
#define COL_NAME_MAX    32     /* longer values are ellipsised    */
#define COL_AUTHORS_MAX 28
#define COL_ELLIPSIS    "..."

/* Width of the step labels in the `fetch` log. */
#define FETCH_LABEL_WIDTH 9

/* Exit codes. */
#define EXIT_OK         0
#define EXIT_NOT_FOUND  1
#define EXIT_FAILED     2
#define EXIT_USAGE      3

typedef struct {
    int bare;      /* machine-readable output, no headers or labels */
    int verbose;   /* extra detail */
    int offline;   /* never touch the network */
} options;

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
        "\n"
        "Options:\n"
        "  -b, --bare       Machine-readable output: paths or tab-separated fields only\n"
        "  -v, --verbose    Print extra detail\n"
        "  -o, --offline    Skip the automatic digest refresh, use the cached copy\n"
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

/* Uppercases the 3-letter tab code for display, as the game shows it. */
static void code_upper(char *out, size_t outsz, const char *code)
{
    size_t i;

    snprintf(out, outsz, "%s", code ? code : "");
    for (i = 0; out[i]; i++) {
        if (out[i] >= 'a' && out[i] <= 'z')
            out[i] = (char)(out[i] - 'a' + 'A');
    }
}

/* Keeps the "YYYY-MM-DD" part of an ISO 8601 timestamp. */
static void date_short(char *out, size_t outsz, const char *iso)
{
    if (iso && strlen(iso) >= DIGEST_DATE_LEN)
        snprintf(out, outsz, "%.*s", DIGEST_DATE_LEN, iso);
    else
        snprintf(out, outsz, "%s", iso ? iso : "");
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
        char code[16], name[256], authors[256], date[32];

        code_upper(code, sizeof code, dig->tabs[i].code);
        fit_column(name, sizeof name, dig->tabs[i].name, COL_NAME_MAX);
        fit_column(authors, sizeof authors, dig->tabs[i].authors, COL_AUTHORS_MAX);
        date_short(date, sizeof date, dig->tabs[i].date);

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

    code_upper(upper, sizeof upper, tab->code);
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
    code_upper(upper, sizeof upper, tab->code);

    /* The game's own folders. */
    if (npp_find_game_dirs(&paths, err, sizeof err) != 0) {
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
        code_upper(other, sizeof other, installed_code);
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

    if (tab_install(dig, tab, &paths, &report, err, sizeof err) != 0) {
        fprintf(stderr, TABBER_NAME ": %s could not be installed: %s\n", upper, err);
        install_report_free(&report);
        goto done;
    }

    /* Files the tab ships that the game has no use for. */
    for (i = 0; i < report.skipped.count; i++)
        fprintf(stderr, TABBER_NAME ": warning: '%s' is not a level or challenge file "
                        "the game reads, skipped\n", report.skipped.items[i]);

    log_step("target", "%s", report.game_levels_dir);
    log_step("installed", "%lu file(s), %lu skipped",
             (unsigned long)report.installed_count, (unsigned long)report.skipped.count);
    log_step("originals", "kept alongside with the '%s' suffix", INSTALL_BACKUP_SUFFIX);
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

    code_upper(upper, sizeof upper, code);
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

int main(int argc, char **argv)
{
    options opts = {0};
    const char *command = NULL;
    const char *argument = NULL;
    int i;

    plat_init();

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_usage(stdout);
            return EXIT_OK;
        } else if (!strcmp(arg, "-V") || !strcmp(arg, "--version")) {
            printf(TABBER_NAME " " TABBER_VERSION " (" PLAT_NAME ")\n");
            return EXIT_OK;
        } else if (!strcmp(arg, "-b") || !strcmp(arg, "--bare")) {
            opts.bare = 1;
        } else if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
            opts.verbose = 1;
        } else if (!strcmp(arg, "-o") || !strcmp(arg, "--offline")) {
            opts.offline = 1;
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

    if (!command || !strcmp(command, "paths"))
        return cmd_paths(&opts);
    if (!strcmp(command, "list"))
        return cmd_list(&opts);
    if (!strcmp(command, "update"))
        return cmd_update(&opts);
    if (!strcmp(command, "fetch"))
        return cmd_fetch(&opts, argument);
    if (!strcmp(command, "remove"))
        return cmd_remove(&opts, argument);
    if (!strcmp(command, "install"))
        return cmd_install(&opts, argument);

    fprintf(stderr, TABBER_NAME ": unknown command '%s'\n", command);
    print_usage(stderr);
    return EXIT_USAGE;
}
