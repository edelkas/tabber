/*
 * tabber - custom tab installer for N++.
 *
 * Current scope: locate the game's directories, and keep the catalogue of
 * available custom tabs (the digest) up to date. Later stages (level/challenge
 * swapping, library patching, palettes, savefile, texts) build on both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "digest.h"
#include "paths.h"
#include "platform.h"
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

/* ---- Entry point ------------------------------------------------------- */

int main(int argc, char **argv)
{
    options opts = {0};
    const char *command = "paths";
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
        } else if (!strcmp(arg, "paths") || !strcmp(arg, "list") || !strcmp(arg, "update")) {
            command = arg;
        } else {
            fprintf(stderr, TABBER_NAME ": unknown argument '%s'\n", arg);
            print_usage(stderr);
            return EXIT_USAGE;
        }
    }

    if (!strcmp(command, "list"))
        return cmd_list(&opts);
    if (!strcmp(command, "update"))
        return cmd_update(&opts);
    return cmd_paths(&opts);
}
