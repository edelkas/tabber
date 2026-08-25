/*
 * paths.h - Location of N++'s installation and personal directories.
 *
 * The robust route on PC goes through Steam: registry (or default location)
 * -> Steam folder -> libraryfolders.vdf -> the library holding N++ ->
 * appmanifest_230270.acf -> installdir. The personal folder, which holds the
 * savefile and editor levels, lives outside the installation and is derived
 * from per-platform user directories instead.
 */
#ifndef TABBER_PATHS_H
#define TABBER_PATHS_H

#include <stddef.h>

#include "platform.h"

/* ---- Steam identifiers and layout -------------------------------------- */

#define NPP_STEAM_APPID         "230270"                 /* N++'s Steam app ID */
#define STEAM_APPS_SUBDIR       "steamapps"
#define STEAM_COMMON_SUBDIR     "common"
#define STEAM_LIBFOLDERS_FILE   "libraryfolders.vdf"
#define STEAM_APPMANIFEST_FMT   "appmanifest_" NPP_STEAM_APPID ".acf"

/* KeyValues keys read out of those files. */
#define KVK_LIBRARYFOLDERS      "libraryfolders"
#define KVK_PATH                "path"
#define KVK_APPS                "apps"
#define KVK_APPSTATE            "AppState"
#define KVK_INSTALLDIR          "installdir"

/* Steam's launcher, used to sanity-check a candidate Steam folder. */
#ifdef _WIN32
#  define STEAM_LAUNCHER        "steam.exe"
#elif defined(__APPLE__)
#  define STEAM_LAUNCHER        "steam_osx"
#else
#  define STEAM_LAUNCHER        "steam.sh"
#endif

/* Windows registry locations holding Steam's install path. */
#define STEAM_REG_SUBKEY_WOW64  "SOFTWARE\\Wow6432Node\\Valve\\Steam"
#define STEAM_REG_SUBKEY_NATIVE "SOFTWARE\\Valve\\Steam"
#define STEAM_REG_SUBKEY_USER   "Software\\Valve\\Steam"
#define STEAM_REG_VALUE_INSTALL "InstallPath"
#define STEAM_REG_VALUE_PATH    "SteamPath"

/* ---- N++ layout -------------------------------------------------------- */

#define NPP_DEFAULT_INSTALLDIR  "N++"     /* fallback for AppState->installdir */
#define NPP_ASSETS_SUBDIR       "NPP"     /* assets folder, marks a real install */

/*
 * The main library, holding the game's core code. It sits in the installation
 * root, except on macOS where it lives inside the application bundle.
 */
#ifdef _WIN32
#  define NPP_MAIN_LIBRARY      "npp.dll"
#elif defined(__APPLE__)
#  define NPP_MAIN_LIBRARY      "libnpp.dylib"
#  define NPP_BUNDLE_SUBDIR     "N++.app/Contents/Frameworks"
#else
#  define NPP_MAIN_LIBRARY      "libnpp.so"
#endif
#define NPP_PERSONAL_VENDOR     "Metanet" /* personal folder: <base>/Metanet/N++ */
#define NPP_PERSONAL_GAME       "N++"

/* Default per-platform roots, expanded against the environment at runtime. */
#define WIN_ENV_PROGFILES_X86   "ProgramFiles(x86)"
#define WIN_ENV_PROGFILES       "ProgramFiles"
#define STEAM_DEFAULT_DIRNAME   "Steam"
/* XDG_ENV_DATA_HOME and XDG_DEFAULT_DATA_HOME come from platform.h: they are
 * facts about the system, not about Steam, and the tool's own root uses them. */
#define LINUX_STEAM_ALT_1       ".steam/steam"                   /* legacy symlinks */
#define LINUX_STEAM_ALT_2       ".steam/root"
#define LINUX_STEAM_FLATPAK     ".var/app/com.valvesoftware.Steam/data/Steam"
#define MACOS_STEAM_SUPPORT     "Library/Application Support/Steam"

/* Proton prefix that Windows builds running under Linux write their data to. */
#define PROTON_PERSONAL_SUFFIX  "Steam/steamapps/compatdata/" NPP_STEAM_APPID \
                                "/pfx/drive_c/users/steamuser/My Documents"

/*
 * Overrides the whole Steam lookup with a ready-made installation directory.
 * Meant for tests and for copies of the game Steam does not know about.
 */
#define TABBER_ENV_GAME_DIR "TABBER_GAME_DIR"

/* The same for the personal folder, which holds the savefile. */
#define TABBER_ENV_PERSONAL_DIR "TABBER_PERSONAL_DIR"

/* ...and for Steam's own folder, which holds the cloud saves. */
#define TABBER_ENV_STEAM_DIR "TABBER_STEAM_DIR"

/* ---- Discovery results ------------------------------------------------- */

typedef struct {
    char *steam_dir;     /* Steam's base folder (its main library)        */
    char *library_dir;   /* the Steam library folder that contains N++    */
    char *install_dir;   /* N++'s installation directory                  */
    char *personal_dir;  /* N++'s personal directory (savefile, levels)   */
} npp_paths;

/*
 * Fill steam_dir, library_dir and install_dir. Returns 0 on success, -1 on
 * failure with a reason in `err`; fields that could be resolved before the
 * failure are still set.
 */
int npp_find_game_dirs(npp_paths *paths, char *err, size_t errsz);

/* Fill personal_dir. Returns 0 on success, -1 on failure with a reason in `err`. */
int npp_find_personal_dir(npp_paths *paths, char *err, size_t errsz);

/* Release every string owned by `paths` and zero it. */
void npp_paths_free(npp_paths *paths);

#endif /* TABBER_PATHS_H */
