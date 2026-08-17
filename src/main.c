/*
 * tabber - custom tab installer for N++.
 *
 * Current scope: locate the game's installation and personal directories.
 * Later stages (level/challenge swapping, library patching, palettes,
 * savefile, texts) build on the same path discovery.
 */
#include <stdio.h>
#include <string.h>

#include "paths.h"
#include "platform.h"
#include "util.h"

#define TABBER_NAME     "tabber"
#define TABBER_VERSION  "0.1.0"

/* Labels used by the default (human-readable) output. */
#define LABEL_STEAM     "Steam folder:       "
#define LABEL_LIBRARY   "Steam library:      "
#define LABEL_INSTALL   "Installation dir:   "
#define LABEL_PERSONAL  "Personal dir:       "

/* Exit codes. */
#define EXIT_OK         0
#define EXIT_NOT_FOUND  1
#define EXIT_USAGE      3

typedef struct {
    int bare;      /* print paths only, one per line, for scripting */
    int verbose;   /* also print the Steam folder and library */
} options;

static void print_usage(FILE *out)
{
    fprintf(out,
        "Usage: " TABBER_NAME " [options] [paths]\n"
        "\n"
        "Commands:\n"
        "  paths            Locate N++'s installation and personal directories (default)\n"
        "\n"
        "Options:\n"
        "  -b, --bare       Print the paths only, one per line (installation first)\n"
        "  -v, --verbose    Also print the Steam folder and the Steam library used\n"
        "  -h, --help       Show this help and exit\n"
        "  -V, --version    Show the version and exit\n");
}

static void print_path(const char *label, const char *path, const options *opts)
{
    if (opts->bare)
        printf("%s\n", path);
    else
        printf("%s%s\n", label, path);
}

/* Locates and prints the game directories; returns the process exit code. */
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

int main(int argc, char **argv)
{
    options opts = {0};
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
        } else if (!strcmp(arg, "paths")) {
            /* The only command so far, and the default one. */
        } else {
            fprintf(stderr, TABBER_NAME ": unknown argument '%s'\n", arg);
            print_usage(stderr);
            return EXIT_USAGE;
        }
    }

    return cmd_paths(&opts);
}
