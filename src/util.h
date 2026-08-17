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

/* Writes a formatted message into `err` if it is non-NULL. */
void err_set(char *err, size_t errsz, const char *fmt, ...);

#endif /* TABBER_UTIL_H */
