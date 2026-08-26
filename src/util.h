/*
 * util.h - Allocation, string and error-reporting helpers used tool-wide.
 *
 * All strings handled by the tool are NUL-terminated UTF-8. Allocation
 * failures are fatal: this is a short-lived CLI tool, so there is nothing
 * useful to do but abort.
 */
#ifndef TABBER_UTIL_H
#define TABBER_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Recommended size for the caller-provided error buffers used across the tool. */
#define TB_ERR_LEN 512

/* Allocators that abort on out-of-memory. */
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);

/* Returns a copy of `s` (NULL if `s` is NULL). Caller frees. */
char *str_dup(const char *s);

/* Returns a copy of `s` with ASCII letters lowercased. Caller frees. */
char *str_dup_lower(const char *s);

/* A copy of [start, end) without the whitespace at either end. Caller frees. */
char *str_trim_copy(const char *start, const char *end);

/* printf-style formatting into a freshly allocated string. Caller frees. */
char *str_fmt(const char *fmt, ...);

/* ASCII case-insensitive equality; NULL-safe (NULL only equals NULL). */
int str_ieq(const char *a, const char *b);

/*
 * Number of characters a UTF-8 string occupies on screen, i.e. its codepoint
 * count. Used to align table columns, which byte counts would get wrong.
 */
size_t str_display_width(const char *s);

/* Writes a formatted message into `err` if it is non-NULL. */
void err_set(char *err, size_t errsz, const char *fmt, ...);

/*
 * CRC-32 (IEEE 802.3), the checksum both ZIP and gzip carry. Used to judge
 * whether the bytes that came out of a decompressor are the ones that went in.
 */
unsigned long crc32_bytes(const void *data, size_t len);

/* Length of an ISO 8601 UTC timestamp, "YYYY-MM-DDTHH:MM:SSZ". */
#define TB_TIMESTAMP_LEN 20

/* Current UTC time as an ISO 8601 timestamp, the format the digest uses. */
void time_now_iso8601(char *out, size_t outsz);

/*
 * The same, `hours` in the past. Timestamps in this format are fixed-width
 * UTC, so strcmp orders them: a stamp that compares below this one is older
 * than `hours`, which is all "is it time to check again?" needs.
 */
void time_ago_iso8601(int hours, char *out, size_t outsz);

/*
 * Back the other way: a timestamp in that format as a Unix time in UTC, or 0
 * when it is not one. Anything after the seconds is ignored, so the fractional
 * stamps some digests carry parse the same as ours.
 */
long long time_from_iso8601(const char *iso);

/* Longest string the two below write, terminator included. */
#define TB_WHEN_LEN 32

/*
 * How long ago `when` was, for reading rather than sorting: "just now", "5
 * minutes ago", "3 days ago". `now` is passed in rather than read from the
 * clock so a caller stamping a whole list gets one consistent answer, and so
 * this can be tested. A `when` of 0, or one in the future, gives `never`,
 * which may be NULL for an empty string.
 */
void time_relative(long long when, long long now, const char *never,
                   char *out, size_t outsz);

/* A local date and time, "YYYY-MM-DD HH:MM", for showing an exact moment. */
void time_local_stamp(long long when, char *out, size_t outsz);

/* ---- Growable list of owned strings ------------------------------------ */

typedef struct {
    char **items;
    size_t count, cap;
} str_list;

/* Appends `s`, taking ownership. A NULL is ignored, which lets failed lookups
 * be pushed directly. */
void str_list_push(str_list *list, char *s);

/* Sorts in ASCII order, so output does not depend on directory order. */
void str_list_sort(str_list *list);

/* Case-insensitive membership test. */
int str_list_contains(const str_list *list, const char *s);

void str_list_free(str_list *list);

/* ---- Text as lines ------------------------------------------------------ */

/*
 * A text file split into lines, each keeping the terminator it carried, so one
 * line can be rewritten and every other byte handed back exactly as it was.
 * The game's own files are ours to edit, not to reformat.
 */
typedef struct {
    str_list lines;   /* the content of each line, terminator stripped */
    str_list ends;    /* "\n", "\r\n", or "" on a file ending mid-line */
} text_lines;

void text_lines_split(const char *text, size_t len, text_lines *out);

/* Joins them back together. `len_out` (optional) receives the byte count. */
char *text_lines_join(const text_lines *lines, size_t *len_out);

/* Replaces line `index`, taking ownership of `line`. */
void text_lines_set(text_lines *lines, size_t index, char *line);

void text_lines_free(text_lines *lines);

/* ---- Growable byte buffer ---------------------------------------------- */

typedef struct {
    char *data;
    size_t len, cap;
} byte_buf;

void buf_append(byte_buf *buf, const void *data, size_t len);

/* NUL-terminates and hands the buffer over to the caller, who must free it. */
char *buf_finish(byte_buf *buf, size_t *len_out);

void buf_free(byte_buf *buf);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_UTIL_H */
