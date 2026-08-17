#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void err_set(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (!err || errsz == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}
