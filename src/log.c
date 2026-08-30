/*
 * log.c - Implementation of the message log. See log.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "platform.h"
#include "util.h"

/* The lines, oldest first once it has wrapped, and where the next one goes. */
static log_entry g_lines[LOG_HISTORY];
static size_t g_count;
static size_t g_next;
static int g_echo = 1;

/* Whether the lines are kept on disk, and where. The path is worked out once
 * and held for the life of the process: it cannot change under us. */
static int g_saving;
static char *g_path;

/* Whether `c` is one of the characters a line is folded on. */
static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/*
 * Copies `in` to `out` as a single line: every run of whitespace becomes one
 * space, and the ends are trimmed. What does not fit is cut, on a character
 * boundary rather than in the middle of a UTF-8 sequence, and the ellipsis put
 * in its place.
 */
static void fold(const char *in, char *out, size_t outsz)
{
    size_t len = 0, room = outsz - sizeof LOG_ELLIPSIS;   /* both count the NUL */

    while (*in && is_space(*in))
        in++;
    for (; *in; in++) {
        char c = is_space(*in) ? ' ' : *in;

        /* One space for a run of them, and none at all at the end: a trailing
         * space is only known to be one once something follows it. */
        if (c == ' ' && (len == 0 || out[len - 1] == ' '))
            continue;
        if (len >= room) {
            while (len > 0 && ((unsigned char)out[len - 1] & 0xC0) == 0x80)
                len--;                    /* back off a half-written character */
            if (len > 0 && out[len - 1] == ' ')
                len--;
            memcpy(out + len, LOG_ELLIPSIS, sizeof LOG_ELLIPSIS);
            return;
        }
        out[len++] = c;
    }
    while (len > 0 && out[len - 1] == ' ')
        len--;
    out[len] = '\0';
}

/*
 * Sets the file aside under LOG_OLD_SUFFIX once it has grown past the cap,
 * replacing whatever was there. What it leaves is two files: the one being
 * written and the one before it, and never a third.
 */
static void roll_over(void)
{
    char *old = str_fmt("%s%s", g_path, LOG_OLD_SUFFIX);

    plat_replace_file(g_path, old);
    free(old);
}

/*
 * Appends one line to the file, dated as well as timed: it holds more than one
 * sitting, so the hour alone would not say which day it happened on. Anything
 * that goes wrong here is passed over in silence — a line about what the tool
 * did is not worth a complaint about where it could not be written.
 */
static void write_to_file(const log_entry *entry)
{
    char stamp[TB_STAMP_LEN];
    long size;
    FILE *f;

    if (!g_path)
        g_path = log_file_path();
    if (!g_path)
        return;
    f = plat_fopen(g_path, "ab");
    if (!f)
        return;
    time_local_full(entry->when, stamp, sizeof stamp);
    fprintf(f, "%s  %s\n", stamp, entry->text);

    /* The size is asked of the handle that has just written it rather than of
     * the file, so the cap costs no second look at the disk. */
    size = ftell(f);
    fclose(f);
    if (size >= LOG_FILE_MAX)
        roll_over();
}

void log_linev(const char *fmt, va_list ap)
{
    /* Room to form the message before it is folded: what arrives with newlines
     * in it is longer than the one line it becomes. */
    char raw[LOG_LINE_MAX * 2];
    log_entry *entry = &g_lines[g_next];

    vsnprintf(raw, sizeof raw, fmt, ap);
    fold(raw, entry->text, sizeof entry->text);
    if (entry->text[0] == '\0')
        return;                  /* nothing was said, so nothing is recorded */

    entry->when = (long long)time(NULL);
    g_next = (g_next + 1) % LOG_HISTORY;
    if (g_count < LOG_HISTORY)
        g_count++;

    /* Flushed, so that a line and the error a caller writes to stderr right
     * after it reach a redirected output in the order they happened. */
    if (g_echo && plat_has_output()) {
        fputs(entry->text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    if (g_saving)
        write_to_file(entry);
}

void log_line(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_linev(fmt, ap);
    va_end(ap);
}

const log_entry *log_at(size_t age)
{
    if (age >= g_count)
        return NULL;
    /* g_next is one past the newest, and the ring is LOG_HISTORY long. */
    return &g_lines[(g_next + LOG_HISTORY - 1 - age) % LOG_HISTORY];
}

const log_entry *log_last(void)
{
    return log_at(0);
}

size_t log_count(void)
{
    return g_count;
}

void log_clear(void)
{
    g_count = 0;
    g_next = 0;
}

void log_set_echo(int on)
{
    g_echo = on;
}

void log_set_saving(int on)
{
    g_saving = on;
}

char *log_file_path(void)
{
    char *root = plat_app_root();
    char *path;

    if (!root)
        return NULL;      /* nowhere of ours to write to: nothing is kept */
    path = path_join(root, LOG_FILENAME);
    free(root);
    return path;
}

int log_file_exists(void)
{
    /* The one already worked out when there is one: this is asked on opening
     * a window, and the answer is a file to open rather than a file to write,
     * so it is asked whether or not anything is being kept. */
    char *path = g_path ? g_path : log_file_path();
    FILE *f;
    int there;

    if (!path)
        return 0;
    f = plat_fopen(path, "rb");
    there = f != NULL;
    if (f)
        fclose(f);
    if (path != g_path)
        free(path);
    return there;
}
