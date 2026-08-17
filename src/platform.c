#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "util.h"

/* Cap for the buffer growth of plat_read_file: no game file we read is bigger. */
#define PLAT_MAX_FILE_SIZE (64u * 1024u * 1024u)

/* ====================================================================== */
/*  Windows                                                               */
/* ====================================================================== */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>   /* SHGetKnownFolderPath, FOLDERID_Documents */

#ifdef _MSC_VER
#  pragma comment(lib, "advapi32.lib")   /* registry */
#  pragma comment(lib, "ole32.lib")      /* CoTaskMemFree */
#  pragma comment(lib, "shell32.lib")    /* SHGetKnownFolderPath */
#  pragma comment(lib, "uuid.lib")       /* FOLDERID_* GUIDs */
#endif

/* Prefix that GetFinalPathNameByHandleW prepends to the paths it returns. */
#define WIN_LONG_PREFIX     L"\\\\?\\"
#define WIN_LONG_PREFIX_UNC L"\\\\?\\UNC\\"

/* Converts a UTF-16 string to a freshly allocated UTF-8 one. */
static char *utf8_from_wide(const wchar_t *w)
{
    int bytes;
    char *out;

    if (!w)
        return NULL;
    bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return NULL;
    out = xmalloc((size_t)bytes);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, bytes, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* Converts a UTF-8 string to a freshly allocated UTF-16 one. */
static wchar_t *wide_from_utf8(const char *s)
{
    int chars;
    wchar_t *out;

    if (!s)
        return NULL;
    chars = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (chars <= 0)
        return NULL;
    out = xmalloc((size_t)chars * sizeof(wchar_t));
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, chars) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

void plat_init(void)
{
    /* Make the console render the UTF-8 we print. */
    SetConsoleOutputCP(CP_UTF8);
}

char *plat_getenv(const char *name)
{
    wchar_t *wname, *wbuf;
    DWORD len;
    char *out;

    wname = wide_from_utf8(name);
    if (!wname)
        return NULL;

    len = GetEnvironmentVariableW(wname, NULL, 0);   /* includes the NUL */
    if (len == 0) {
        free(wname);
        return NULL;
    }
    wbuf = xmalloc((size_t)len * sizeof(wchar_t));
    if (GetEnvironmentVariableW(wname, wbuf, len) == 0) {
        free(wname);
        free(wbuf);
        return NULL;
    }
    free(wname);

    out = utf8_from_wide(wbuf);
    free(wbuf);
    if (out && out[0] == '\0') {   /* treat empty as unset */
        free(out);
        return NULL;
    }
    return out;
}

/* Wrapper around the known-folder API, returning UTF-8. */
static char *win_known_folder(REFKNOWNFOLDERID id)
{
    wchar_t *wpath = NULL;
    char *out = NULL;

    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &wpath))) {
        out = utf8_from_wide(wpath);
        CoTaskMemFree(wpath);
    }
    return out;
}

char *plat_home_dir(void)
{
    char *home = plat_getenv("USERPROFILE");
    if (home)
        return home;
    return win_known_folder(&FOLDERID_Profile);
}

char *plat_documents_dir(void)
{
    char *docs = win_known_folder(&FOLDERID_Documents);
    char *home;

    if (docs)
        return docs;

    /* Fallback for the (unlikely) case the shell API fails. */
    home = plat_home_dir();
    if (!home)
        return NULL;
    docs = path_join(home, "Documents");
    free(home);
    return docs;
}

char *plat_exe_dir(void)
{
    wchar_t *wbuf = NULL;
    DWORD cap = MAX_PATH, len;
    char *path, *dir;

    /* GetModuleFileNameW truncates instead of reporting the size: grow blindly. */
    for (;;) {
        wbuf = xrealloc(wbuf, cap * sizeof(wchar_t));
        len = GetModuleFileNameW(NULL, wbuf, cap);
        if (len == 0) {
            free(wbuf);
            return NULL;
        }
        if (len < cap)
            break;
        cap *= 2;
    }

    path = utf8_from_wide(wbuf);
    free(wbuf);
    if (!path)
        return NULL;
    dir = path_dirname(path);
    free(path);
    return dir;
}

int plat_replace_file(const char *src, const char *dst)
{
    wchar_t *wsrc = wide_from_utf8(src);
    wchar_t *wdst = wide_from_utf8(dst);
    int rc = -1;

    if (wsrc && wdst)
        rc = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
    free(wsrc);
    free(wdst);
    return rc;
}

int plat_remove_file(const char *path)
{
    wchar_t *wpath = wide_from_utf8(path);
    int rc = -1;

    if (wpath)
        rc = DeleteFileW(wpath) ? 0 : -1;
    free(wpath);
    return rc;
}

/* Shared attribute query for plat_is_dir / plat_is_file. */
static DWORD win_attrs(const char *path)
{
    wchar_t *wpath = wide_from_utf8(path);
    DWORD attrs;

    if (!wpath)
        return INVALID_FILE_ATTRIBUTES;
    attrs = GetFileAttributesW(wpath);
    free(wpath);
    return attrs;
}

int plat_is_dir(const char *path)
{
    DWORD attrs = win_attrs(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_is_file(const char *path)
{
    DWORD attrs = win_attrs(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

FILE *plat_fopen(const char *path, const char *mode)
{
    wchar_t *wpath = wide_from_utf8(path);
    wchar_t *wmode = wide_from_utf8(mode);
    FILE *f = NULL;

    if (wpath && wmode)
        f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}

char *plat_canonical_path(const char *path)
{
    wchar_t *wpath = wide_from_utf8(path);
    wchar_t *wfull = NULL;
    HANDLE h;
    DWORD len;
    char *out = NULL;

    if (!wpath)
        return NULL;

    /* BACKUP_SEMANTICS is what allows opening a directory handle. */
    h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    len = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len > 0) {
        wfull = xmalloc((size_t)len * sizeof(wchar_t));
        if (GetFinalPathNameByHandleW(h, wfull, len, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) == 0) {
            free(wfull);
            wfull = NULL;
        }
    }
    CloseHandle(h);
    if (!wfull)
        return NULL;

    /* Strip the "\\?\" prefix; UNC paths come back as "\\?\UNC\server\share". */
    if (wcsncmp(wfull, WIN_LONG_PREFIX_UNC, wcslen(WIN_LONG_PREFIX_UNC)) == 0) {
        wchar_t *unc = wfull + wcslen(WIN_LONG_PREFIX_UNC) - 2;   /* reuse two chars as "\\" */
        unc[0] = L'\\';
        unc[1] = L'\\';
        out = utf8_from_wide(unc);
    } else if (wcsncmp(wfull, WIN_LONG_PREFIX, wcslen(WIN_LONG_PREFIX)) == 0) {
        out = utf8_from_wide(wfull + wcslen(WIN_LONG_PREFIX));
    } else {
        out = utf8_from_wide(wfull);
    }
    free(wfull);
    return out;
}

char *plat_reg_read_str(plat_reg_hive hive, const char *subkey, const char *value)
{
    HKEY root = (hive == PLAT_REG_HKLM) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    wchar_t *wsub = wide_from_utf8(subkey);
    wchar_t *wval = wide_from_utf8(value);
    wchar_t *wdata = NULL;
    DWORD size = 0;
    char *out = NULL;

    if (!wsub || !wval)
        goto done;

    /* Two-pass query: first the required size, then the data itself. */
    if (RegGetValueW(root, wsub, wval, RRF_RT_REG_SZ, NULL, NULL, &size) != ERROR_SUCCESS || size == 0)
        goto done;
    wdata = xmalloc(size);
    if (RegGetValueW(root, wsub, wval, RRF_RT_REG_SZ, NULL, wdata, &size) != ERROR_SUCCESS)
        goto done;

    out = utf8_from_wide(wdata);

done:
    free(wsub);
    free(wval);
    free(wdata);
    if (out && out[0] == '\0') {
        free(out);
        out = NULL;
    }
    return out;
}

/* ====================================================================== */
/*  POSIX (Linux, macOS)                                                  */
/* ====================================================================== */
#else

#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#  include <stdint.h>
#endif

void plat_init(void)
{
    /* Nothing to do: POSIX consoles are UTF-8 already. */
}

char *plat_getenv(const char *name)
{
    const char *v = getenv(name);
    if (!v || v[0] == '\0')
        return NULL;
    return str_dup(v);
}

char *plat_home_dir(void)
{
    char *home = plat_getenv("HOME");
    struct passwd *pw;

    if (home)
        return home;
    pw = getpwuid(getuid());   /* fallback for environments without $HOME */
    if (pw && pw->pw_dir && pw->pw_dir[0])
        return str_dup(pw->pw_dir);
    return NULL;
}

char *plat_documents_dir(void)
{
    char *home = plat_home_dir();
    char *docs;

    if (!home)
        return NULL;
    docs = path_join(home, "Documents");
    free(home);
    return docs;
}

char *plat_exe_dir(void)
{
    char *path = NULL, *dir;

#if defined(__APPLE__)
    {
        uint32_t size = 0;
        _NSGetExecutablePath(NULL, &size);   /* first call reports the size */
        if (size == 0)
            return NULL;
        path = xmalloc(size);
        if (_NSGetExecutablePath(path, &size) != 0) {
            free(path);
            return NULL;
        }
    }
#else
    {
        size_t cap = 256;
        for (;;) {
            ssize_t len;
            path = xrealloc(path, cap);
            len = readlink("/proc/self/exe", path, cap - 1);
            if (len < 0) {
                free(path);
                return NULL;
            }
            if ((size_t)len < cap - 1) {
                path[len] = '\0';
                break;
            }
            cap *= 2;   /* truncated: try again with more room */
        }
    }
#endif

    dir = path_dirname(path);
    free(path);
    return dir;
}

int plat_replace_file(const char *src, const char *dst)
{
    return rename(src, dst) == 0 ? 0 : -1;   /* POSIX rename replaces atomically */
}

int plat_remove_file(const char *path)
{
    return unlink(path) == 0 ? 0 : -1;
}

int plat_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int plat_is_file(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

FILE *plat_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

char *plat_canonical_path(const char *path)
{
    char *resolved = realpath(path, NULL);
    char *out;

    if (!resolved)
        return NULL;
    out = str_dup(resolved);
    free(resolved);
    return out;
}

#endif /* _WIN32 */

/* ====================================================================== */
/*  Portable helpers                                                      */
/* ====================================================================== */

char *plat_read_file(const char *path, size_t *len_out)
{
    FILE *f;
    char *buf;
    size_t cap, len = 0;

    f = plat_fopen(path, "rb");
    if (!f)
        return NULL;

    /* Read incrementally: file sizes reported by stat can lie (e.g. on procfs). */
    cap = 8192;
    buf = xmalloc(cap);
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len - 1, f);
        len += got;
        if (got == 0)
            break;
        if (len + 1 >= cap) {
            if (cap >= PLAT_MAX_FILE_SIZE)
                break;
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    if (ferror(f)) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);

    buf[len] = '\0';
    if (len_out)
        *len_out = len;
    return buf;
}

int plat_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = plat_fopen(path, "wb");
    size_t written;

    if (!f)
        return -1;
    written = len ? fwrite(data, 1, len, f) : 0;
    if (fclose(f) != 0 || written != len)
        return -1;
    return 0;
}

char *path_dirname(const char *path)
{
    const char *slash = NULL, *p;

    if (!path)
        return NULL;
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            slash = p;
    }
    if (!slash)
        return str_dup(".");           /* no directory part */
    if (slash == path)
        return str_dup(PATH_SEP_STR);  /* the root itself */
    return str_fmt("%.*s", (int)(slash - path), path);
}

char *path_join(const char *base, const char *leaf)
{
    size_t base_len;
    char *out;

    if (!base || base[0] == '\0')
        return str_dup(leaf);
    if (!leaf || leaf[0] == '\0')
        return str_dup(base);

    /* Avoid doubling separators where the two components meet. */
    base_len = strlen(base);
    while (base_len > 0 && (base[base_len - 1] == '/' || base[base_len - 1] == '\\'))
        base_len--;
    while (*leaf == '/' || *leaf == '\\')
        leaf++;

    out = str_fmt("%.*s%c%s", (int)base_len, base, PATH_SEP, leaf);
    return out;
}

void path_to_native(char *path)
{
    size_t len;

    if (!path)
        return;

#ifdef _WIN32
    /* Steam stores forward slashes in some registry values; normalize them. */
    {
        char *p;
        for (p = path; *p; p++) {
            if (*p == '/')
                *p = PATH_SEP;
        }
    }
#endif

    /* Drop trailing separators, but never shorten a root such as "C:\" or "/". */
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        if (len == 3 && path[1] == ':')
            break;
        path[--len] = '\0';
    }
}

int path_is_absolute(const char *path)
{
    if (!path || path[0] == '\0')
        return 0;
#ifdef _WIN32
    if ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'))
        return 1;   /* UNC */
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
    return path[0] == '/';
#endif
}
