#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        fputs("tabber: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    if (!p) {
        fputs("tabber: out of memory\n", stderr);
        exit(2);
    }
    return p;
}

char *str_dup(const char *s)
{
    size_t len;
    char *copy;

    if (!s)
        return NULL;
    len = strlen(s);
    copy = xmalloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *str_dup_lower(const char *s)
{
    char *copy = str_dup(s);
    char *p;

    for (p = copy; p && *p; p++) {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    }
    return copy;
}

char *str_trim_copy(const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    return str_fmt("%.*s", (int)(end - start), start);
}

/* Formats into a right-sized buffer: vsnprintf first reports the length needed. */
static char *str_vfmt(const char *fmt, va_list ap)
{
    va_list ap2;
    int len;
    char *buf;

    va_copy(ap2, ap);
    len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len < 0)
        return str_dup("");

    buf = xmalloc((size_t)len + 1);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    return buf;
}

char *str_fmt(const char *fmt, ...)
{
    va_list ap;
    char *out;

    va_start(ap, fmt);
    out = str_vfmt(fmt, ap);
    va_end(ap);
    return out;
}

int str_ieq(const char *a, const char *b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

size_t str_display_width(const char *s)
{
    size_t width = 0;

    if (!s)
        return 0;
    for (; *s; s++) {
        /* Skip UTF-8 continuation bytes: they belong to the previous glyph. */
        if (((unsigned char)*s & 0xC0) != 0x80)
            width++;
    }
    return width;
}

void err_set(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (!err || errsz == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Formats a point in time the way the digest and the state file write them. */
static void time_iso8601(time_t when, char *out, size_t outsz)
{
    struct tm *utc = gmtime(&when);

    if (!utc || strftime(out, outsz, "%Y-%m-%dT%H:%M:%SZ", utc) == 0)
        snprintf(out, outsz, "1970-01-01T00:00:00Z");   /* clock unavailable */
}

void time_now_iso8601(char *out, size_t outsz)
{
    time_iso8601(time(NULL), out, outsz);
}

void time_ago_iso8601(int hours, char *out, size_t outsz)
{
    time_t now = time(NULL);
    time_t then = now - (time_t)hours * 3600;

    time_iso8601(then < 0 ? 0 : then, out, outsz);
}

/*
 * Days from 1970-01-01 to a civil date, by Howard Hinnant's algorithm. Doing
 * the arithmetic ourselves keeps this free of timegm(), which Windows does not
 * have, and of the local timezone, which would make the answer depend on where
 * the machine thinks it is.
 */
static long long days_from_civil(long y, unsigned m, unsigned d)
{
    long era;
    unsigned yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);                       /* [0, 399]   */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* [0, 365]   */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return (long long)era * 146097 + (long long)doe - 719468;
}

long long time_from_iso8601(const char *iso)
{
    int y, mo, d, h, mi, sec;

    if (!iso || sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d",
                       &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60)
        return 0;
    return days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400LL +
           h * 3600LL + mi * 60LL + sec;
}

/* Thresholds of the relative form, in seconds, coarsest unit last. */
#define WHEN_MINUTE  60LL
#define WHEN_HOUR    3600LL
#define WHEN_DAY     86400LL
#define WHEN_MONTH   2629746LL    /* the average Gregorian month */
#define WHEN_YEAR    31556952LL   /* ...and year, 365.2425 days  */

void time_relative(long long when, long long now, const char *never,
                   char *out, size_t outsz)
{
    long long ago = now - when;
    long long n;
    const char *unit;

    if (!out || outsz == 0)
        return;
    /* A stamp in the future is a clock that disagrees with ours, not a thing
     * that has not happened yet; it reads better as no answer than as one. */
    if (when <= 0 || ago < 0) {
        snprintf(out, outsz, "%s", never ? never : "");
        return;
    }

    if (ago < WHEN_MINUTE) {
        snprintf(out, outsz, "just now");
        return;
    }
    if (ago < WHEN_HOUR)       { n = ago / WHEN_MINUTE; unit = "minute"; }
    else if (ago < WHEN_DAY)   { n = ago / WHEN_HOUR;   unit = "hour";   }
    else if (ago < WHEN_MONTH) { n = ago / WHEN_DAY;    unit = "day";    }
    else if (ago < WHEN_YEAR)  { n = ago / WHEN_MONTH;  unit = "month";  }
    else                       { n = ago / WHEN_YEAR;   unit = "year";   }

    snprintf(out, outsz, "%ld %s%s ago", (long)n, unit, n == 1 ? "" : "s");
}

void time_local_stamp(long long when, char *out, size_t outsz)
{
    time_t t = (time_t)when;
    struct tm *local;

    if (!out || outsz == 0)
        return;
    local = localtime(&t);
    if (when <= 0 || !local || strftime(out, outsz, "%Y-%m-%d %H:%M", local) == 0)
        snprintf(out, outsz, "unknown");
}

void time_local_clock(long long when, char *out, size_t outsz)
{
    time_t t = (time_t)when;
    struct tm *local;

    if (!out || outsz == 0)
        return;
    local = localtime(&t);

    /* Dashes rather than "unknown": these line up down the left of a list, and
     * a word among them would push every line beside it out of true. */
    if (when <= 0 || !local || strftime(out, outsz, "%H:%M:%S", local) == 0)
        snprintf(out, outsz, "--:--:--");
}

void str_list_push(str_list *list, char *s)
{
    if (!s)
        return;
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 8;
        list->items = xrealloc(list->items, list->cap * sizeof(*list->items));
    }
    list->items[list->count++] = s;
}

static int str_list_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void str_list_sort(str_list *list)
{
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(*list->items), str_list_cmp);
}

int str_list_contains(const str_list *list, const char *s)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (str_ieq(list->items[i], s))
            return 1;
    }
    return 0;
}

void str_list_free(str_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

void text_lines_split(const char *text, size_t len, text_lines *out)
{
    size_t start = 0, i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i <= len; i++) {
        size_t end;

        if (i < len && text[i] != '\n')
            continue;
        end = i;
        if (i == len && start == len && len > 0)
            break;                          /* the text ended on a terminator */
        if (end > start && text[end - 1] == '\r')
            end--;
        str_list_push(&out->lines, str_fmt("%.*s", (int)(end - start), text + start));
        str_list_push(&out->ends, str_dup(i == len ? "" : end < i ? "\r\n" : "\n"));
        start = i + 1;
    }
}

char *text_lines_join(const text_lines *lines, size_t *len_out)
{
    byte_buf out = {0};
    size_t i;

    for (i = 0; i < lines->lines.count; i++) {
        buf_append(&out, lines->lines.items[i], strlen(lines->lines.items[i]));
        buf_append(&out, lines->ends.items[i], strlen(lines->ends.items[i]));
    }
    return buf_finish(&out, len_out);
}

void text_lines_set(text_lines *lines, size_t index, char *line)
{
    if (index >= lines->lines.count) {
        free(line);
        return;
    }
    free(lines->lines.items[index]);
    lines->lines.items[index] = line;
}

void text_lines_free(text_lines *lines)
{
    str_list_free(&lines->lines);
    str_list_free(&lines->ends);
}

void buf_append(byte_buf *buf, const void *data, size_t len)
{
    if (len == 0)
        return;
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 1024;
        while (cap < buf->len + len + 1)   /* +1 keeps room for a terminator */
            cap *= 2;
        buf->data = xrealloc(buf->data, cap);
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

char *buf_finish(byte_buf *buf, size_t *len_out)
{
    char *data;

    if (!buf->data) {
        buf->data = xmalloc(1);
        buf->cap = 1;
    }
    buf->data[buf->len] = '\0';
    data = buf->data;
    if (len_out)
        *len_out = buf->len;
    buf->data = NULL;
    buf->len = buf->cap = 0;
    return data;
}

void buf_free(byte_buf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

/* ---- CRC-32 (IEEE 802.3, as used by ZIP and gzip) ---------------------- */

#define CRC32_POLY 0xEDB88320UL

static unsigned long crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void)
{
    unsigned long c;
    int i, bit;

    for (i = 0; i < 256; i++) {
        c = (unsigned long)i;
        for (bit = 0; bit < 8; bit++)
            c = (c & 1) ? (CRC32_POLY ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

unsigned long crc32_bytes(const void *data, size_t len)
{
    const unsigned char *p = data;
    unsigned long crc = 0xFFFFFFFFUL;
    size_t i;

    if (!crc32_ready)
        crc32_init();
    for (i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}
