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

/* The current user's home directory, or NULL if it cannot be determined. */
char *plat_home_dir(void);

/* The user's "Documents" folder (relocation-aware on Windows). */
char *plat_documents_dir(void);

/* Directory holding the running executable, or NULL if it cannot be found. */
char *plat_exe_dir(void);

/* ---- Filesystem -------------------------------------------------------- */

int plat_is_dir(const char *path);
int plat_is_file(const char *path);

/* Opens a file with a UTF-8 path; `mode` is a stdio mode string. */
FILE *plat_fopen(const char *path, const char *mode);

/*
 * Reads a whole file into a NUL-terminated buffer. `*len_out` (optional)
 * receives the byte count, excluding the terminator. NULL on failure.
 */
char *plat_read_file(const char *path, size_t *len_out);

/* Writes `len` bytes to `path`, truncating it. Returns 0 on success. */
int plat_write_file(const char *path, const void *data, size_t len);

/* Renames `src` over `dst`, replacing it atomically. Returns 0 on success. */
int plat_replace_file(const char *src, const char *dst);

int plat_remove_file(const char *path);

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

#endif /* TABBER_PLATFORM_H */
