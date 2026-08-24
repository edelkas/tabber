#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "util.h"

/*
 * Cap for plat_read_file. The savefile is the big one — around 70 MB
 * uncompressed — so the limit sits well above it, and a file that exceeds it
 * is refused rather than handed back short.
 */
#define PLAT_MAX_FILE_SIZE (128u * 1024u * 1024u)

/* ====================================================================== */
/*  Windows                                                               */
/* ====================================================================== */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>   /* SHGetKnownFolderPath, FOLDERID_Documents */
#include <io.h>       /* _isatty, _fileno                          */
#include <stdio.h>

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

int plat_setenv(const char *name, const char *value)
{
    wchar_t *wname = wide_from_utf8(name);
    wchar_t *wvalue = value ? wide_from_utf8(value) : NULL;
    int rc = -1;

    if (wname && (wvalue || !value))
        rc = SetEnvironmentVariableW(wname, wvalue) ? 0 : -1;
    free(wname);
    free(wvalue);
    return rc;
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

char *plat_exe_path(void)
{
    wchar_t *wbuf = NULL;
    DWORD cap = MAX_PATH, len;
    char *path;

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
    return path;
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

int plat_move_dir(const char *src, const char *dst)
{
    wchar_t *wsrc = wide_from_utf8(src);
    wchar_t *wdst = wide_from_utf8(dst);
    int rc = -1;

    /* No MOVEFILE_REPLACE_EXISTING here: it is documented not to work on
     * directories, and a rename-aside has nothing to replace anyway. */
    if (wsrc && wdst)
        rc = MoveFileW(wsrc, wdst) ? 0 : -1;
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

/* Creates a single directory; an existing one counts as success. */
static int plat_mkdir_one(const char *path)
{
    wchar_t *wpath = wide_from_utf8(path);
    int rc = -1;

    if (wpath) {
        if (CreateDirectoryW(wpath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
            rc = 0;
    }
    free(wpath);
    return rc;
}

int plat_remove_tree(const char *path)
{
    WIN32_FIND_DATAW info;
    HANDLE search;
    wchar_t *wpattern, *wpath;
    char *pattern;
    int failures = 0;

    if (!plat_is_dir(path))
        return plat_is_file(path) ? plat_remove_file(path) : 0;

    pattern = path_join(path, "*");
    wpattern = wide_from_utf8(pattern);
    free(pattern);
    if (!wpattern)
        return -1;

    search = FindFirstFileW(wpattern, &info);
    free(wpattern);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char *name, *child;

            if (wcscmp(info.cFileName, L".") == 0 || wcscmp(info.cFileName, L"..") == 0)
                continue;
            name = utf8_from_wide(info.cFileName);
            if (!name) {
                failures++;
                continue;
            }
            child = path_join(path, name);
            free(name);

            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                failures += plat_remove_tree(child) != 0;
            else
                failures += plat_remove_file(child) != 0;
            free(child);
        } while (FindNextFileW(search, &info));
        FindClose(search);
    }

    wpath = wide_from_utf8(path);
    if (!wpath)
        return -1;
    if (!RemoveDirectoryW(wpath))
        failures++;
    free(wpath);
    return failures ? -1 : 0;
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

int plat_list_dir(const char *path, str_list *out)
{
    WIN32_FIND_DATAW info;
    HANDLE search;
    char *pattern = path_join(path, "*");
    wchar_t *wpattern = wide_from_utf8(pattern);

    free(pattern);
    if (!wpattern)
        return -1;
    search = FindFirstFileW(wpattern, &info);
    free(wpattern);
    if (search == INVALID_HANDLE_VALUE)
        return -1;

    do {
        if (wcscmp(info.cFileName, L".") == 0 || wcscmp(info.cFileName, L"..") == 0)
            continue;
        str_list_push(out, utf8_from_wide(info.cFileName));
    } while (FindNextFileW(search, &info));
    FindClose(search);

    str_list_sort(out);
    return 0;
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

/* ---- Running other programs -------------------------------------------- */

int plat_make_executable(const char *path)
{
    (void)path;      /* here it is the extension that makes a file runnable */
    return 0;
}

int plat_is_interactive(void)
{
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
}

/*
 * Appends one argument to a command line the way the C runtime will parse it
 * back: quoted when it holds a space or a quote of its own, and backslashes
 * doubled where a quote follows them. CreateProcess takes the whole line as a
 * single string, so the quoting is ours to get right — and the path of the
 * program itself routinely has a space in it.
 */
static void append_arg(byte_buf *out, const char *arg)
{
    size_t i = 0, slashes;

    if (arg[0] && !strpbrk(arg, " \t\"")) {
        buf_append(out, arg, strlen(arg));
        return;
    }

    buf_append(out, "\"", 1);
    while (arg[i]) {
        for (slashes = 0; arg[i] == '\\'; i++)
            slashes++;
        if (!arg[i]) {                        /* they precede the closing quote */
            while (slashes--)
                buf_append(out, "\\\\", 2);
            break;
        }
        if (arg[i] == '"') {
            while (slashes--)
                buf_append(out, "\\\\", 2);
            buf_append(out, "\\\"", 2);
        } else {
            while (slashes--)
                buf_append(out, "\\", 1);
            buf_append(out, &arg[i], 1);
        }
        i++;
    }
    buf_append(out, "\"", 1);
}

/* Starts `exe`, waits for it, and reports its exit code. */
static int run_child(const char *exe, char *const *args, size_t count, int *status)
{
    byte_buf line = {0};
    char *utf8;
    wchar_t *wexe, *wline;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    size_t i;
    int rc = -1;

    /* Anything of ours still sitting in a buffer has to reach the console
     * before the child starts writing to the same one. */
    fflush(NULL);

    append_arg(&line, exe);
    for (i = 0; i < count; i++) {
        buf_append(&line, " ", 1);
        append_arg(&line, args[i]);
    }
    utf8 = buf_finish(&line, NULL);

    wexe = wide_from_utf8(exe);
    wline = wide_from_utf8(utf8);
    free(utf8);
    if (!wexe || !wline)
        goto done;

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    /* The handles are inherited, so the child writes to our own console. */
    if (!CreateProcessW(wexe, wline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        goto done;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (status)
        *status = (int)code;
    rc = 0;

done:
    free(wexe);
    free(wline);
    return rc;
}

int plat_run_and_wait(const char *exe, char *const *args, size_t count, int *status)
{
    return run_child(exe, args, count, status);
}

/*
 * There is no exec here: a process cannot become another program. The new
 * binary runs as a child instead and its exit code is passed on, which is the
 * difference the caller has to live with.
 */
int plat_restart(const char *exe, char *const *args, size_t count, int *status)
{
    return run_child(exe, args, count, status);
}

/* ====================================================================== */
/*  POSIX (Linux, macOS)                                                  */
/* ====================================================================== */
#else

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

int plat_setenv(const char *name, const char *value)
{
    if (!value)
        return unsetenv(name) == 0 ? 0 : -1;
    return setenv(name, value, 1) == 0 ? 0 : -1;
}

char *plat_exe_path(void)
{
    char *path = NULL;

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

    return path;
}

int plat_replace_file(const char *src, const char *dst)
{
    return rename(src, dst) == 0 ? 0 : -1;   /* POSIX rename replaces atomically */
}

int plat_move_dir(const char *src, const char *dst)
{
    return rename(src, dst) == 0 ? 0 : -1;
}

int plat_remove_file(const char *path)
{
    return unlink(path) == 0 ? 0 : -1;
}

/* Creates a single directory; an existing one counts as success. */
static int plat_mkdir_one(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

int plat_remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *ent;
    int failures = 0;

    if (!plat_is_dir(path))
        return plat_is_file(path) ? plat_remove_file(path) : 0;

    dir = opendir(path);
    if (dir) {
        while ((ent = readdir(dir)) != NULL) {
            char *child;

            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            child = path_join(path, ent->d_name);
            if (plat_is_dir(child))
                failures += plat_remove_tree(child) != 0;
            else
                failures += plat_remove_file(child) != 0;
            free(child);
        }
        closedir(dir);
    }

    if (rmdir(path) != 0)
        failures++;
    return failures ? -1 : 0;
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

int plat_list_dir(const char *path, str_list *out)
{
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (!dir)
        return -1;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        str_list_push(out, str_dup(ent->d_name));
    }
    closedir(dir);

    str_list_sort(out);
    return 0;
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

/* ---- Running other programs -------------------------------------------- */

int plat_make_executable(const char *path)
{
    /* A binary unpacked from a ZIP arrives with no modes worth the name. */
    return chmod(path, 0755) == 0 ? 0 : -1;
}

int plat_is_interactive(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

/* What execv wants: the program's own name, the arguments, and a NULL. */
static char **build_argv(const char *exe, char *const *args, size_t count)
{
    char **argv = xmalloc((count + 2) * sizeof(*argv));
    size_t i;

    argv[0] = (char *)exe;
    for (i = 0; i < count; i++)
        argv[i + 1] = args[i];
    argv[count + 1] = NULL;
    return argv;
}

int plat_run_and_wait(const char *exe, char *const *args, size_t count, int *status)
{
    char **argv = build_argv(exe, args, count);   /* built before the fork */
    pid_t pid;
    int state = 0;

    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        free(argv);
        return -1;
    }
    if (pid == 0) {
        execv(exe, argv);
        _exit(127);                  /* only reached when it will not run */
    }
    free(argv);

    if (waitpid(pid, &state, 0) < 0)
        return -1;
    if (status)
        *status = WIFEXITED(state) ? WEXITSTATUS(state) : -1;
    return 0;
}

int plat_restart(const char *exe, char *const *args, size_t count, int *status)
{
    char **argv = build_argv(exe, args, count);

    (void)status;                    /* a successful exec never comes back */

    /* exec throws away whatever stdio has buffered, so everything printed so
     * far has to be out of the door before it. */
    fflush(NULL);
    execv(exe, argv);
    free(argv);
    return -1;
}

#endif /* _WIN32 */

/* ====================================================================== */
/*  Portable helpers                                                      */
/* ====================================================================== */

char *plat_exe_dir(void)
{
    char *path = plat_exe_path(), *dir;

    if (!path)
        return NULL;
    dir = path_dirname(path);
    free(path);
    return dir;
}

char *plat_read_file(const char *path, size_t *len_out)
{
    FILE *f;
    char *buf;
    size_t cap, len = 0;

    f = plat_fopen(path, "rb");
    if (!f)
        return NULL;

    /*
     * The reported size is only a hint for the first allocation: sizes can lie
     * (procfs) and the file can grow under us, so the reading itself is
     * incremental and stops at what actually comes out.
     */
    cap = 8192;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && (size_t)size < PLAT_MAX_FILE_SIZE)
            cap = (size_t)size + 1;
    }
    rewind(f);

    buf = xmalloc(cap);
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len - 1, f);
        len += got;
        if (got == 0)
            break;
        if (len + 1 >= cap) {
            if (cap >= PLAT_MAX_FILE_SIZE) {
                char extra;
                /* Only refuse if there really is more: a file of exactly this
                 * size is fine, a bigger one must not come back truncated. */
                if (fread(&extra, 1, 1, f) != 0) {
                    fclose(f);
                    free(buf);
                    return NULL;
                }
                break;
            }
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

int plat_write_file_atomic(const char *path, const void *data, size_t len)
{
    char *tmp = str_fmt("%s%s", path, PLAT_TMP_SUFFIX);
    int rc = -1;

    if (plat_write_file(tmp, data, len) != 0)
        goto done;
    if (plat_replace_file(tmp, path) != 0) {
        plat_remove_file(tmp);
        goto done;
    }
    rc = 0;

done:
    free(tmp);
    return rc;
}

int plat_mkdir_p(const char *path)
{
    char *work;
    size_t i;
    int rc = 0;

    if (!path || !*path)
        return -1;
    if (plat_is_dir(path))
        return 0;

    work = str_dup(path);
    path_to_native(work);

    /* Create each ancestor in turn by temporarily cutting the path short.
     * Start at 1 so a leading separator (or "C:") is never treated as a name. */
    for (i = 1; work[i]; i++) {
        if (work[i] != PATH_SEP)
            continue;
#ifdef _WIN32
        if (i == 2 && work[1] == ':')
            continue;   /* "C:\" is a root, not a directory to create */
#endif
        work[i] = '\0';
        if (*work && plat_mkdir_one(work) != 0 && !plat_is_dir(work)) {
            rc = -1;
            break;
        }
        work[i] = PATH_SEP;
    }
    if (rc == 0)
        rc = plat_mkdir_one(work);

    free(work);
    return rc;
}

int plat_copy_tree(const char *src, const char *dst, size_t *files)
{
    str_list entries = {0};
    size_t i;
    int failures = 0;

    if (!plat_is_dir(src) || plat_mkdir_p(dst) != 0)
        return -1;
    if (plat_list_dir(src, &entries) != 0)
        return -1;

    for (i = 0; i < entries.count; i++) {
        char *from = path_join(src, entries.items[i]);
        char *to = path_join(dst, entries.items[i]);

        if (plat_is_dir(from)) {
            failures += plat_copy_tree(from, to, files) != 0;
        } else {
            size_t len = 0;
            char *data = plat_read_file(from, &len);

            if (!data || plat_write_file(to, data, len) != 0)
                failures++;
            else if (files)
                (*files)++;
            free(data);
        }
        free(from);
        free(to);
    }

    str_list_free(&entries);
    return failures ? -1 : 0;
}

char *plat_app_root(void)
{
    char *override = plat_getenv(TABBER_ENV_HOME);
    char *dir;

    if (override) {
        if (plat_is_dir(override))
            return override;
        free(override);   /* names nothing usable: fall back to the executable */
    }
    dir = plat_exe_dir();
    return dir ? dir : str_dup(".");
}

int plat_write_at(const char *path, size_t offset, const void *data, size_t len)
{
    FILE *f = plat_fopen(path, "r+b");   /* update in place, never truncate */
    size_t written;

    if (!f)
        return -1;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    written = fwrite(data, 1, len, f);
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
