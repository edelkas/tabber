/*
 * platform.h - Thin OS abstraction layer (filesystem, environment, registry).
 *
 * Every path crossing this API is UTF-8; the Windows implementation converts
 * to/from UTF-16 internally so that non-ASCII user names work. Returned
 * strings are heap-allocated and owned by the caller.
 */
#ifndef TABBER_PLATFORM_H
#define TABBER_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native path separator. Windows accepts '/' as well, but we emit '\'. */
#ifdef _WIN32
#  define PATH_SEP      '\\'
#  define PATH_SEP_STR  "\\"
#else
#  define PATH_SEP      '/'
#  define PATH_SEP_STR  "/"
#endif

/* Human-readable name of the platform we were built for. */
#ifdef _WIN32
#  define PLAT_NAME "Windows"
#elif defined(__APPLE__)
#  define PLAT_NAME "macOS"
#elif defined(__linux__)
#  define PLAT_NAME "Linux"
#else
#  define PLAT_NAME "unknown"
#endif

/* One-time process setup (currently: UTF-8 console output on Windows). */
void plat_init(void);

/* ---- Environment ------------------------------------------------------- */

/* Value of environment variable `name`, or NULL when unset/empty. */
char *plat_getenv(const char *name);

/*
 * Sets one for this process and anything it starts afterwards, which is how a
 * restart tells the new binary that it is the restarted one. Returns 0 on
 * success; a NULL value clears the variable.
 */
int plat_setenv(const char *name, const char *value);

/* The current user's home directory, or NULL if it cannot be determined. */
char *plat_home_dir(void);

/* The user's "Documents" folder (relocation-aware on Windows). */
char *plat_documents_dir(void);

/* The running executable itself, or NULL if it cannot be found. Caller frees. */
char *plat_exe_path(void);

/* Directory holding the running executable, or NULL if it cannot be found. */
char *plat_exe_dir(void);

/* ---- Running other programs -------------------------------------------- */

/*
 * Marks a file as runnable. A no-op on Windows, where the extension decides;
 * elsewhere it is 0755, since a binary unpacked from a ZIP arrives without it.
 */
int plat_make_executable(const char *path);

/* Whether both ends of the console are a terminal, so a prompt makes sense. */
int plat_is_interactive(void);

/*
 * Runs `exe` with `args` (which does not include the program name) and waits
 * for it. Returns 0 with *status set to its exit code, or -1 when it could not
 * be started at all.
 */
int plat_run_and_wait(const char *exe, char *const *args, size_t count, int *status);

/*
 * Hands this process over to `exe`, which is what an update does once the new
 * binary is in place. On POSIX the image is replaced and this never returns;
 * Windows has no such call, so a child is started and waited for, and *status
 * receives the exit code for the caller to exit with. Returns -1 when the
 * program could not be started, in which case nothing happened.
 */
int plat_restart(const char *exe, char *const *args, size_t count, int *status);

/* Overrides the tool's root directory; mainly for tests and portable setups. */
#define TABBER_ENV_HOME "TABBER_HOME"

/*
 * Where each system sets aside per-user application data, and what the tool
 * calls its folder inside it. The name follows the local convention: capital
 * on Windows and macOS, where it sits among other applications' capitalised
 * folders, lower case under XDG, where everything else is.
 */
#define WIN_ENV_LOCALAPPDATA    "LOCALAPPDATA"
#define MACOS_APP_SUPPORT       "Library/Application Support"  /* under $HOME */
#define XDG_ENV_DATA_HOME       "XDG_DATA_HOME"
#define XDG_DEFAULT_DATA_HOME   ".local/share"                 /* under $HOME */

#ifdef _WIN32
#  define TABBER_DATA_DIRNAME   "Tabber"
#elif defined(__APPLE__)
#  define TABBER_DATA_DIRNAME   "Tabber"
#else
#  define TABBER_DATA_DIRNAME   "tabber"
#endif

/*
 * The system's per-user data directory, without the tool's folder on the end:
 * %LOCALAPPDATA% on Windows, ~/Library/Application Support on macOS, and
 * $XDG_DATA_HOME (or ~/.local/share) elsewhere. NULL if it cannot be worked
 * out at all. Caller frees.
 */
char *plat_data_dir(void);

/*
 * The tool's root: where config.json, the cached digest and the tab store
 * live. That is TABBER_DATA_DIRNAME under plat_data_dir(), created if it is
 * not there yet, so the binary can be moved or replaced without its state
 * following it around. TABBER_ENV_HOME overrides it when it names a directory
 * that exists, and the executable's own directory stands in if the data
 * directory cannot be found or made. Caller frees.
 */
char *plat_app_root(void);

/* ---- Filesystem -------------------------------------------------------- */

int plat_is_dir(const char *path);
int plat_is_file(const char *path);

/*
 * Names of the entries in a directory, sorted, without "." and "..".
 * Returns 0 on success, -1 if the directory cannot be read.
 */
int plat_list_dir(const char *path, str_list *out);

/* Opens a file with a UTF-8 path; `mode` is a stdio mode string. */
FILE *plat_fopen(const char *path, const char *mode);

/*
 * Reads a whole file into a NUL-terminated buffer. `*len_out` (optional)
 * receives the byte count, excluding the terminator. NULL on failure.
 */
char *plat_read_file(const char *path, size_t *len_out);

/* Writes `len` bytes to `path`, truncating it. Returns 0 on success. */
int plat_write_file(const char *path, const void *data, size_t len);

/* Staging name for the write below. */
#define PLAT_TMP_SUFFIX ".tabber-tmp"

/*
 * The same, but staged beside the file and swapped in, so an interrupted write
 * cannot leave a truncated one behind. For the game's own files, where losing
 * the tail of what was there costs more than the write is worth.
 */
int plat_write_file_atomic(const char *path, const void *data, size_t len);

/*
 * Overwrites `len` bytes at `offset` inside an existing file, leaving the rest
 * of it alone. Returns 0 on success.
 */
int plat_write_at(const char *path, size_t offset, const void *data, size_t len);

/* Renames `src` over `dst`, replacing it atomically. Returns 0 on success. */
int plat_replace_file(const char *src, const char *dst);

/*
 * Renames a directory. Unlike plat_replace_file this never replaces anything:
 * `dst` must not exist, which is all a rename-aside needs and all Windows
 * offers for directories. Returns 0 on success.
 */
int plat_move_dir(const char *src, const char *dst);

int plat_remove_file(const char *path);

/* Creates a directory, including any missing parent. Returns 0 on success
 * (also when it already exists). */
int plat_mkdir_p(const char *path);

/*
 * Deletes a directory and everything inside it. Returns 0 on success, and also
 * when `path` does not exist. Handle with care: it recurses.
 */
int plat_remove_tree(const char *path);

/*
 * Copies a directory and everything inside it, creating `dst` and any missing
 * parent. `files` (optional) is incremented by the number of files copied.
 * Returns 0 on success, -1 if any part of it could not be copied.
 */
int plat_copy_tree(const char *src, const char *dst, size_t *files);

/*
 * Moves the entries named in `names` from one directory to another, files and
 * directories alike, by rename where that works and by copy where it does not
 * (the two can be on different volumes). An entry `from` does not have, or
 * that `to` already has, is left alone: this never writes over anything.
 * `moved` (optional) is incremented per entry actually moved. Returns 0 when
 * everything that had to move did, -1 otherwise.
 */
int plat_move_entries(const char *from, const char *to,
                      const char *const *names, size_t count, size_t *moved);

/*
 * Absolute, symlink-free path with the on-disk spelling (letter case
 * included). The path must exist; returns NULL if it cannot be resolved.
 */
char *plat_canonical_path(const char *path);

/* ---- Path strings ------------------------------------------------------ */

/* Joins two path components with a single native separator. Caller frees. */
char *path_join(const char *base, const char *leaf);

/* In place: rewrites separators to the native one and drops trailing ones. */
void path_to_native(char *path);

/* Everything before the last separator of `path`. Caller frees. */
char *path_dirname(const char *path);

int path_is_absolute(const char *path);

/* ---- Windows registry -------------------------------------------------- */
#ifdef _WIN32

typedef enum {
    PLAT_REG_HKLM,  /* HKEY_LOCAL_MACHINE */
    PLAT_REG_HKCU   /* HKEY_CURRENT_USER  */
} plat_reg_hive;

/* Reads a string value from the registry, or NULL if absent. Caller frees. */
char *plat_reg_read_str(plat_reg_hive hive, const char *subkey, const char *value);

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* TABBER_PLATFORM_H */
