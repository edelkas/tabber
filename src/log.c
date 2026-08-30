/*
 * log.c - Implementation of the message log. See log.h.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "platform.h"

/* The lines, oldest first once it has wrapped, and where the next one goes. */
static log_entry g_lines[LOG_HISTORY];
static size_t g_count;
static size_t g_next;
static int g_echo = 1;

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
