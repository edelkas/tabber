#include <stdlib.h>
#include <string.h>

#include "kv.h"
#include "paths.h"
#include "platform.h"
#include "util.h"

/* ---- Small helpers ----------------------------------------------------- */

static char *join3(const char *a, const char *b, const char *c)
{
    char *ab = path_join(a, b);
    char *abc = path_join(ab, c);
    free(ab);
    return abc;
}

/* Joins `leaf` onto $HOME, or returns NULL if there is no home directory. */
static char *home_join(const char *leaf)
{
    char *home = plat_home_dir();
    char *out;

    if (!home)
        return NULL;
    out = path_join(home, leaf);
    free(home);
    return out;
}

/*
 * Turns a raw path into the form we hand out: native separators and, when the
 * directory exists, the canonical on-disk spelling.
 */
static char *finalize_dir(const char *raw)
{
    char *out = str_dup(raw);
    char *canonical;

    if (!out)
        return NULL;
    path_to_native(out);
    canonical = plat_canonical_path(out);
    if (canonical) {
        free(out);
        return canonical;
    }
    return out;
}

/* ---- Steam base folder ------------------------------------------------- */

/* A Steam folder must exist and hold either the launcher or a steamapps dir. */
static int steam_dir_ok(const char *dir)
{
    char *launcher, *apps;
    int ok;

    if (!dir || !plat_is_dir(dir))
        return 0;
    launcher = path_join(dir, STEAM_LAUNCHER);
    apps = path_join(dir, STEAM_APPS_SUBDIR);
    ok = plat_is_file(launcher) || plat_is_dir(apps);
    free(launcher);
    free(apps);
    return ok;
}

/* Builds the ordered list of places Steam may live in, best guess first. */
static void collect_steam_candidates(str_list *out)
{
#ifdef _WIN32
    /* The registry covers non-default install locations, so it comes first. */
    str_list_push(out, plat_reg_read_str(PLAT_REG_HKLM, STEAM_REG_SUBKEY_WOW64,
                                     STEAM_REG_VALUE_INSTALL));
    str_list_push(out, plat_reg_read_str(PLAT_REG_HKLM, STEAM_REG_SUBKEY_NATIVE,
                                     STEAM_REG_VALUE_INSTALL));
    str_list_push(out, plat_reg_read_str(PLAT_REG_HKCU, STEAM_REG_SUBKEY_USER,
                                     STEAM_REG_VALUE_PATH));

    /* Default locations: %PROGRAMFILES(X86)%\Steam on 64-bit, %PROGRAMFILES%\Steam on 32-bit. */
    {
        char *progfiles = plat_getenv(WIN_ENV_PROGFILES_X86);
        if (progfiles) {
            str_list_push(out, path_join(progfiles, STEAM_DEFAULT_DIRNAME));
            free(progfiles);
        }
        progfiles = plat_getenv(WIN_ENV_PROGFILES);
        if (progfiles) {
            str_list_push(out, path_join(progfiles, STEAM_DEFAULT_DIRNAME));
            free(progfiles);
        }
    }

#elif defined(__APPLE__)
    str_list_push(out, home_join(MACOS_STEAM_SUPPORT));

#else /* Linux, Steam Deck */
    {
        char *xdg = plat_getenv(XDG_ENV_DATA_HOME);
        if (xdg) {
            str_list_push(out, path_join(xdg, STEAM_DEFAULT_DIRNAME));
            free(xdg);
        } else {
            str_list_push(out, home_join(XDG_DEFAULT_DATA_HOME "/" STEAM_DEFAULT_DIRNAME));
        }
        str_list_push(out, home_join(LINUX_STEAM_ALT_1));
        str_list_push(out, home_join(LINUX_STEAM_ALT_2));
        str_list_push(out, home_join(LINUX_STEAM_FLATPAK));
    }
#endif
}

/* Locates Steam's base folder, which doubles as its main library folder. */
static char *find_steam_dir(char *err, size_t errsz)
{
    str_list candidates = {0};
    char *found = NULL;
    size_t i;

    collect_steam_candidates(&candidates);
    for (i = 0; i < candidates.count && !found; i++) {
        char *dir = str_dup(candidates.items[i]);
        path_to_native(dir);
        if (steam_dir_ok(dir))
            found = finalize_dir(dir);
        free(dir);
    }

    if (!found) {
        err_set(err, errsz, "could not locate Steam (checked %u location(s) on %s)",
                (unsigned)candidates.count, PLAT_NAME);
    }
    str_list_free(&candidates);
    return found;
}

/* ---- Steam library folder holding N++ ---------------------------------- */

/* Path of N++'s app manifest inside a given library folder. Caller frees. */
static char *appmanifest_path(const char *library_dir)
{
    return join3(library_dir, STEAM_APPS_SUBDIR, STEAM_APPMANIFEST_FMT);
}

/* A library holds N++ when it carries the game's app manifest. */
static int library_has_npp(const char *library_dir)
{
    char *manifest;
    int ok;

    if (!library_dir || !plat_is_dir(library_dir))
        return 0;
    manifest = appmanifest_path(library_dir);
    ok = plat_is_file(manifest);
    free(manifest);
    return ok;
}

/*
 * Walks libraryfolders.vdf looking for the library that holds N++: first the
 * libraries that list the app ID under "apps", then any other one carrying the
 * manifest. Falls back to the Steam folder itself when the file is unusable.
 */
static char *find_library_dir(const char *steam_dir, char *err, size_t errsz)
{
    char *vdf_path;
    char kv_err[TB_ERR_LEN];
    kv_node *root;
    const kv_node *libs, *lib;
    str_list candidates = {0};
    char *found = NULL;
    size_t i;

    /* The Steam folder is always an implicit library candidate. */
    str_list_push(&candidates, str_dup(steam_dir));

    vdf_path = join3(steam_dir, STEAM_APPS_SUBDIR, STEAM_LIBFOLDERS_FILE);
    root = kv_parse_file(vdf_path, kv_err, sizeof kv_err);
    if (root) {
        libs = kv_child(root, KVK_LIBRARYFOLDERS);
        for (lib = libs ? libs->children : NULL; lib; lib = lib->next) {
            const char *path = kv_value(lib, KVK_PATH);
            const kv_node *apps;
            char *dir;

            if (!path)
                continue;
            dir = str_dup(path);
            path_to_native(dir);

            /* When the library advertises its apps, trust that listing first. */
            apps = kv_child(lib, KVK_APPS);
            if (apps && kv_child(apps, NPP_STEAM_APPID) && library_has_npp(dir)) {
                found = finalize_dir(dir);
                free(dir);
                break;
            }
            str_list_push(&candidates, dir);
        }
        kv_free(root);
    }

    /* No advertised match: probe every known library for the app manifest. */
    for (i = 0; i < candidates.count && !found; i++) {
        if (library_has_npp(candidates.items[i]))
            found = finalize_dir(candidates.items[i]);
    }

    if (!found) {
        err_set(err, errsz, "N++ (app %s) is not installed in any Steam library listed in '%s'",
                NPP_STEAM_APPID, vdf_path);
    }
    free(vdf_path);
    str_list_free(&candidates);
    return found;
}

/* ---- Installation directory -------------------------------------------- */

/* Reads AppState->installdir from the app manifest; NULL if unavailable. */
static char *read_installdir(const char *library_dir)
{
    char *manifest = appmanifest_path(library_dir);
    char kv_err[TB_ERR_LEN];
    kv_node *root = kv_parse_file(manifest, kv_err, sizeof kv_err);
    char *installdir = NULL;

    if (root) {
        const char *value = kv_value(kv_child(root, KVK_APPSTATE), KVK_INSTALLDIR);
        if (value && value[0])
            installdir = str_dup(value);
        kv_free(root);
    }
    free(manifest);
    return installdir;
}

/* An installation must exist and contain the game's assets folder. */
static int install_dir_ok(const char *dir)
{
    char *assets;
    int ok;

    if (!dir || !plat_is_dir(dir))
        return 0;
    assets = path_join(dir, NPP_ASSETS_SUBDIR);
    ok = plat_is_dir(assets);
    free(assets);
    return ok;
}

int npp_find_game_dirs(npp_paths *paths, char *err, size_t errsz)
{
    char *installdir, *candidate;
    int ok;

    /* Steam's own folder can be named on its own, which is all the cloud
     * saves need; it is also what the game-directory override leaves out. */
    {
        char *override = plat_getenv(TABBER_ENV_STEAM_DIR);

        if (override) {
            path_to_native(override);
            if (plat_is_dir(override))
                paths->steam_dir = finalize_dir(override);
            free(override);
        }
    }

    /* An explicit installation directory short-circuits the Steam lookup. */
    {
        char *override = plat_getenv(TABBER_ENV_GAME_DIR);
        if (override) {
            path_to_native(override);
            ok = install_dir_ok(override);
            if (ok)
                paths->install_dir = finalize_dir(override);
            else
                err_set(err, errsz, "%s points at '%s', which is not an N++ installation "
                                    "(no '%s' folder)",
                        TABBER_ENV_GAME_DIR, override, NPP_ASSETS_SUBDIR);
            free(override);
            return ok ? 0 : -1;
        }
    }

    if (!paths->steam_dir)
        paths->steam_dir = find_steam_dir(err, errsz);
    if (!paths->steam_dir)
        return -1;

    paths->library_dir = find_library_dir(paths->steam_dir, err, errsz);
    if (!paths->library_dir)
        return -1;

    /* The manifest names the subfolder of steamapps/common holding the game. */
    installdir = read_installdir(paths->library_dir);
    if (!installdir)
        installdir = str_dup(NPP_DEFAULT_INSTALLDIR);
    path_to_native(installdir);

    if (path_is_absolute(installdir)) {
        candidate = str_dup(installdir);   /* unusual, but honour it if present */
    } else {
        char *common = join3(paths->library_dir, STEAM_APPS_SUBDIR, STEAM_COMMON_SUBDIR);
        candidate = path_join(common, installdir);
        free(common);
    }
    free(installdir);

    ok = install_dir_ok(candidate);
    if (ok)
        paths->install_dir = finalize_dir(candidate);
    else
        err_set(err, errsz, "'%s' does not look like an N++ installation (no '%s' folder)",
                candidate, NPP_ASSETS_SUBDIR);
    free(candidate);
    return ok ? 0 : -1;
}

/* ---- Personal directory ------------------------------------------------ */

/* Ordered list of the places the personal folder may live in on this platform. */
static void collect_personal_candidates(str_list *out)
{
#ifdef _WIN32
    /* %USERPROFILE%\Documents, resolved through the shell in case it moved. */
    {
        char *docs = plat_documents_dir();
        if (docs) {
            str_list_push(out, path_join(docs, NPP_PERSONAL_VENDOR));
            free(docs);
        }
    }

#elif defined(__APPLE__)
    str_list_push(out, home_join("Documents/" NPP_PERSONAL_VENDOR));

#else /* Linux, Steam Deck */
    {
        char *xdg = plat_getenv(XDG_ENV_DATA_HOME);
        char *data_home = xdg ? str_dup(xdg) : home_join(XDG_DEFAULT_DATA_HOME);
        free(xdg);

        if (data_home) {
            /* Native build. */
            str_list_push(out, path_join(data_home, NPP_PERSONAL_VENDOR));
            /* Windows build running under Proton, which writes into its prefix. */
            str_list_push(out, join3(data_home, PROTON_PERSONAL_SUFFIX, NPP_PERSONAL_VENDOR));
            free(data_home);
        }
        str_list_push(out, home_join("Documents/" NPP_PERSONAL_VENDOR));
    }
#endif
}

int npp_find_personal_dir(npp_paths *paths, char *err, size_t errsz)
{
    str_list candidates = {0};
    char *override = plat_getenv(TABBER_ENV_PERSONAL_DIR);
    size_t i;

    /* An explicit personal folder short-circuits the search. */
    if (override) {
        path_to_native(override);
        if (plat_is_dir(override)) {
            paths->personal_dir = finalize_dir(override);
            free(override);
            return 0;
        }
        err_set(err, errsz, "%s points at '%s', which is not a directory",
                TABBER_ENV_PERSONAL_DIR, override);
        free(override);
        return -1;
    }

    collect_personal_candidates(&candidates);
    for (i = 0; i < candidates.count && !paths->personal_dir; i++) {
        char *dir = path_join(candidates.items[i], NPP_PERSONAL_GAME);
        path_to_native(dir);
        if (plat_is_dir(dir))
            paths->personal_dir = finalize_dir(dir);
        free(dir);
    }

    if (!paths->personal_dir) {
        char *first = candidates.count
                    ? path_join(candidates.items[0], NPP_PERSONAL_GAME)
                    : str_dup("(unknown)");
        err_set(err, errsz, "could not find N++'s personal folder (expected e.g. '%s'); "
                            "has the game been run at least once?", first);
        free(first);
    }
    str_list_free(&candidates);
    return paths->personal_dir ? 0 : -1;
}

/* ---- Lifetime ---------------------------------------------------------- */

void npp_paths_free(npp_paths *paths)
{
    free(paths->steam_dir);
    free(paths->library_dir);
    free(paths->install_dir);
    free(paths->personal_dir);
    memset(paths, 0, sizeof *paths);
}
