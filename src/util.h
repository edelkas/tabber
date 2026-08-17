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

/* Recommended size for the caller-provided error buffers used across the tool. */
#define TB_ERR_LEN 512

/* Allocators that abort on out-of-memory. */
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);

/* Returns a copy of `s` (NULL if `s` is NULL). Caller frees. */
char *str_dup(const char *s);

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

/* Length of an ISO 8601 UTC timestamp, "YYYY-MM-DDTHH:MM:SSZ". */
#define TB_TIMESTAMP_LEN 20

/* Current UTC time as an ISO 8601 timestamp, the format the digest uses. */
void time_now_iso8601(char *out, size_t outsz);

/* ---- Growable byte buffer ---------------------------------------------- */

typedef struct {
    char *data;
    size_t len, cap;
} byte_buf;

void buf_append(byte_buf *buf, const void *data, size_t len);

/* NUL-terminates and hands the buffer over to the caller, who must free it. */
char *buf_finish(byte_buf *buf, size_t *len_out);

void buf_free(byte_buf *buf);

#endif /* TABBER_UTIL_H */
