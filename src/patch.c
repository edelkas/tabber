#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "install.h"
#include "patch.h"
#include "platform.h"
#include "util.h"

char *lib_path(const npp_paths *paths)
{
#ifdef __APPLE__
    /* On macOS the library lives inside the application bundle. */
    char *bundle = path_join(paths->install_dir, NPP_BUNDLE_SUBDIR);
    char *file = path_join(bundle, NPP_MAIN_LIBRARY);

    free(bundle);
    return file;
#else
    return path_join(paths->install_dir, NPP_MAIN_LIBRARY);
#endif
}

/* ---- Searching --------------------------------------------------------- */

/* Offset of `needle` in `haystack` at or after `from`, or (size_t)-1. */
static size_t mem_find(const char *haystack, size_t hlen,
                       const char *needle, size_t nlen, size_t from)
{
    size_t i;

    if (nlen == 0 || hlen < nlen)
        return (size_t)-1;
    for (i = from; i + nlen <= hlen; i++) {
        if (haystack[i] == needle[0] && memcmp(haystack + i, needle, nlen) == 0)
            return i;
    }
    return (size_t)-1;
}

/* How many times `needle` occurs, and where it first does. */
static int mem_count(const char *haystack, size_t hlen, const char *needle,
                     size_t *first_out)
{
    size_t nlen = strlen(needle);
    size_t at = mem_find(haystack, hlen, needle, nlen, 0);
    int count = 0;

    if (first_out)
        *first_out = at;
    while (at != (size_t)-1) {
        count++;
        at = mem_find(haystack, hlen, needle, nlen, at + nlen);
    }
    return count;
}

/*
 * Reads the URI region starting at `offset`: the text up to the first NUL, or
 * up to the end of the region.
 */
static void read_region(const lib_image *img, size_t offset, char *out, size_t outsz)
{
    size_t i = 0, limit = LIB_URI_BUDGET;

    if (offset + limit > img->size)
        limit = img->size - offset;
    while (i < limit && i + 1 < outsz && img->data[offset + i] != '\0') {
        out[i] = img->data[offset + i];
        i++;
    }
    out[i] = '\0';
}

/*
 * Pulls the tab code out of a patched URI: everything after the first '/' that
 * follows the server part. Empty when the URI carries no path.
 */
static void extract_code(const char *uri, const char *base, char *out, size_t outsz)
{
    size_t base_len = strlen(base);
    const char *rest;
    size_t i = 0;

    out[0] = '\0';
    if (strlen(uri) <= base_len || uri[base_len] != '/')
        return;
    rest = uri + base_len + 1;
    while (rest[i] && rest[i] != '/' && i + 1 < outsz) {
        out[i] = rest[i];
        i++;
    }
    out[i] = '\0';
}

int lib_open(const npp_paths *paths, const str_list *known, lib_image *img,
             char *err, size_t errsz)
{
    size_t i, at;

    memset(img, 0, sizeof(*img));
    img->state = LIB_UNKNOWN;
    img->region = LIB_URI_BUDGET;
    img->path = lib_path(paths);

    if (!plat_is_file(img->path)) {
        err_set(err, errsz, "the game's main library is missing at '%s'", img->path);
        free(img->path);
        img->path = NULL;
        return -1;
    }

    img->data = plat_read_file(img->path, &img->size);
    if (!img->data) {
        err_set(err, errsz, "cannot read '%s'", img->path);
        free(img->path);
        img->path = NULL;
        return -1;
    }

    /* The official URI means the library is untouched. */
    img->occurrences = mem_count(img->data, img->size, LIB_OFFICIAL_URI, &at);
    if (img->occurrences > 0) {
        img->state = LIB_ORIGINAL;
        img->offset = at;
        snprintf(img->uri, sizeof img->uri, "%s", LIB_OFFICIAL_URI);
        return 0;
    }

    /* Otherwise look for any URI a patch could have written. */
    for (i = 0; known && i < known->count; i++) {
        const char *candidate = known->items[i];

        img->occurrences = mem_count(img->data, img->size, candidate, &at);
        if (img->occurrences == 0)
            continue;

        img->state = LIB_PATCHED;
        img->offset = at;
        snprintf(img->base, sizeof img->base, "%s", candidate);
        read_region(img, at, img->uri, sizeof img->uri);
        extract_code(img->uri, candidate, img->code, sizeof img->code);
        return 0;
    }

    img->occurrences = 0;
    return 0;   /* readable, but pointing somewhere we do not recognise */
}

void lib_close(lib_image *img)
{
    free(img->path);
    free(img->data);
    img->path = NULL;
    img->data = NULL;
    img->size = 0;
}

/* ---- Writing ----------------------------------------------------------- */

int lib_write_uri(const lib_image *img, const char *uri, char *err, size_t errsz)
{
    char region[LIB_URI_BUDGET];
    size_t len = strlen(uri);

    if (len > LIB_URI_BUDGET) {
        err_set(err, errsz, "'%s' is %u bytes, more than the %u available",
                uri, (unsigned)len, (unsigned)LIB_URI_BUDGET);
        return -1;
    }

    /* The rest of the region becomes NUL bytes, which also terminates it. */
    memset(region, 0, sizeof region);
    memcpy(region, uri, len);

    if (plat_write_at(img->path, img->offset, region, sizeof region) != 0) {
        err_set(err, errsz, "cannot write to '%s'; check the file's permissions "
                            "and that the game is not running", img->path);
        return -1;
    }
    return 0;
}

/* ---- The developer credit ---------------------------------------------- */

char *lib_credit_text(const char *authors)
{
    byte_buf out = {0};
    char *text;
    size_t i, len = 0;

    for (i = 0; authors && authors[i] && len < LIB_CREDIT_BUDGET; i++) {
        unsigned char c = (unsigned char)authors[i];

        /* Printable ASCII and nothing else: the game's font cannot draw the
         * rest, which is why the installers before this one dropped it too. */
        if (c >= 0x20 && c < 0x7f) {
            buf_append(&out, &authors[i], 1);
            len++;
        }
    }
    text = buf_finish(&out, NULL);
    if (!text)
        return NULL;

    /* Cutting to the budget can leave a trailing space, which would show up
     * in the game looking like a mistake. */
    while (len > 0 && text[len - 1] == ' ')
        text[--len] = '\0';
    if (!text[0]) {
        free(text);
        return NULL;
    }
    return text;
}

int lib_credit_is_original(const lib_image *img)
{
    return mem_count(img->data, img->size, LIB_CREDIT_ORIGINAL, NULL) == 1;
}

int lib_write_credit(lib_image *img, const char *current, const char *replacement,
                     char *err, size_t errsz)
{
    char region[LIB_CREDIT_BUDGET];
    size_t at = (size_t)-1, len, i;
    int found;

    if (!current || !*current || !replacement || !*replacement) {
        err_set(err, errsz, "there is no credit text to write");
        return -1;
    }
    len = strlen(replacement);
    if (len > LIB_CREDIT_BUDGET) {
        err_set(err, errsz, "'%s' is %u bytes, more than the %u available",
                replacement, (unsigned)len, (unsigned)LIB_CREDIT_BUDGET);
        return -1;
    }

    /* The credit can only be found by what it says, there being nothing else
     * to go on: no length, no marker, and an offset that moves between builds. */
    found = mem_count(img->data, img->size, current, &at);
    if (found == 0) {
        err_set(err, errsz, "'%s' is not in '%s'", current, img->path);
        return -1;
    }
    if (found > 1) {
        err_set(err, errsz, "'%s' appears %d times in '%s', so which one is the "
                            "credit cannot be told", current, found, img->path);
        return -1;
    }
    if (at + LIB_CREDIT_BUDGET > img->size) {
        err_set(err, errsz, "the credit in '%s' runs past the end of the file",
                img->path);
        return -1;
    }

    /*
     * Before overwriting a whole region of it: a NUL in front, so it is the
     * start of a string, and nothing but padding behind it, so the bytes we
     * are about to take belong to nobody else. Anything else is a match that
     * happened to fall somewhere in the code.
     */
    if (at == 0 || img->data[at - 1] != '\0') {
        err_set(err, errsz, "what looks like the credit in '%s' is part of "
                            "something else", img->path);
        return -1;
    }
    for (i = strlen(current); i < LIB_CREDIT_BUDGET; i++) {
        if (img->data[at + i] != '\0') {
            err_set(err, errsz, "the credit in '%s' is followed by something that "
                                "would be overwritten", img->path);
            return -1;
        }
    }

    memset(region, 0, sizeof region);
    memcpy(region, replacement, len);
    if (plat_write_at(img->path, at, region, sizeof region) != 0) {
        err_set(err, errsz, "cannot write to '%s'; check the file's permissions "
                            "and that the game is not running", img->path);
        return -1;
    }
    /* Keep the copy in memory in step, so writing again finds the new text. */
    memcpy(img->data + at, region, sizeof region);
    return 0;
}

/* ---- Building the URI to patch in -------------------------------------- */

char *lib_build_uri(const server_addr *addr, const char *code, size_t budget,
                    char *err, size_t errsz)
{
    char *base, *uri;

    /* Preferred form: keep the scheme, so the URI says what it means. */
    base = server_uri(addr, 1);
    uri = str_fmt("%s/%s", base, code);
    free(base);
    if (strlen(uri) <= budget)
        return uri;
    free(uri);

    /*
     * Too long. Dropping the scheme is only safe for HTTP, which is what the
     * game assumes when a URI has none.
     */
    if (addr->scheme[0] && !str_ieq(addr->scheme, SCHEME_HTTP)) {
        err_set(err, errsz, "the server URI for '%s://%s' does not fit in %u bytes "
                            "and its scheme cannot be dropped",
                addr->scheme, addr->host, (unsigned)budget);
        return NULL;
    }

    base = server_uri(addr, 0);
    uri = str_fmt("%s/%s", base, code);
    free(base);
    if (strlen(uri) <= budget)
        return uri;

    err_set(err, errsz, "the server URI '%s' does not fit in the %u bytes the game "
                        "reserves for it", uri, (unsigned)budget);
    free(uri);
    return NULL;
}

/* ---- Health check ------------------------------------------------------ */

int lib_check(config *cfg, const digest *dig, const npp_paths *paths,
              lib_health *out, char *err, size_t errsz)
{
    str_list known = {0};
    lib_image img;
    char installed[TAB_CODE_MAX_LEN + 1];
    int has_installed;

    memset(out, 0, sizeof(*out));

    server_known_uris(cfg, dig, &known);
    if (lib_open(paths, &known, &img, err, errsz) != 0) {
        str_list_free(&known);
        return -1;
    }

    has_installed = install_detect(cfg, paths, installed, sizeof installed);
    snprintf(out->state_code, sizeof out->state_code, "%s", has_installed ? installed : "");
    snprintf(out->lib_code, sizeof out->lib_code, "%s", img.code);
    snprintf(out->uri, sizeof out->uri, "%s", img.uri);
    out->state = img.state;

    /* The library and the state file have to tell the same story. */
    if (img.state == LIB_UNKNOWN) {
        err_set(out->detail, sizeof out->detail,
                "the library points at no URI we recognise; it may have been patched "
                "by another tool");
    } else if (img.occurrences > 1) {
        err_set(out->detail, sizeof out->detail,
                "the server URI appears %d times in the library, expected once",
                img.occurrences);
    } else if (!has_installed && img.state == LIB_PATCHED) {
        /*
         * A tab is installed that this tabber did not install: the mark of an
         * installer that came before it, which kept no state of its own. That
         * is a state to recognise rather than a fault to report — uninstalling
         * has to work from here, which is the whole point of being able to
         * read what the older installers left behind.
         */
        err_set(out->detail, sizeof out->detail,
                "the library is patched for '%s', which nothing here recorded "
                "installing; an older installer left it", img.code[0] ? img.code : "?");
        out->unrecorded = 1;
        out->healthy = 1;
    } else if (has_installed && img.state == LIB_ORIGINAL) {
        err_set(out->detail, sizeof out->detail,
                "'%s' is recorded as installed but the library still points at the "
                "official server", installed);
    } else if (has_installed && !str_ieq(img.code, installed)) {
        err_set(out->detail, sizeof out->detail,
                "the library is patched for '%s' but '%s' is recorded as installed",
                img.code[0] ? img.code : "?", installed);
    } else {
        out->healthy = 1;
    }

    /* Record the verdict, every time the check runs. */
    config_set_state_library(cfg, out->healthy);

    lib_close(&img);
    str_list_free(&known);
    return 0;
}
