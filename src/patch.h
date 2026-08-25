/*
 * patch.h - Redirecting the game's server queries to a 3rd party server.
 *
 * The main library carries the server URI as a plain string. Patching means
 * overwriting it in place with the 3rd party URI plus the tab's code as the
 * first path component (e.g. "outte.ovh:8126/ctp"), padded with NUL bytes to
 * the exact length of the original. Nothing else in the binary moves, so the
 * change is reversible by writing the original URI back.
 *
 * The replacement therefore has to fit in the original's length, which is why
 * the "http://" prefix may be dropped: the game assumes plain HTTP when a URI
 * carries no scheme. That matters most when the host is an address literal.
 */
#ifndef TABBER_PATCH_H
#define TABBER_PATCH_H

#include <stddef.h>

#include "config.h"
#include "digest.h"
#include "paths.h"
#include "server.h"
#include "tabs.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The URI the game ships with. Its length is the budget for a patched one. */
#define LIB_OFFICIAL_URI    "https://dojo.nplusplus.ninja"
#define LIB_URI_BUDGET      (sizeof(LIB_OFFICIAL_URI) - 1)

/*
 * The developer credit the game renders, which the older installers replaced
 * with the tab's author. Same story as the URI: a plain string overwritten in
 * place, its own length the budget, and the original written back to undo it.
 */
#define LIB_CREDIT_ORIGINAL "Metanet Software"
#define LIB_CREDIT_BUDGET   (sizeof(LIB_CREDIT_ORIGINAL) - 1)

/* Longest URI text we keep around, padding excluded. */
#define LIB_URI_MAX         128

/* What the library currently points at. */
typedef enum {
    LIB_ORIGINAL,   /* the official URI: no tab installed          */
    LIB_PATCHED,    /* a 3rd party URI: some tab is installed      */
    LIB_UNKNOWN     /* neither was found: we cannot tell           */
} lib_state;

/* The library file, loaded and inspected. */
typedef struct {
    char *path;
    char *data;
    size_t size;
    lib_state state;
    size_t offset;                     /* start of the URI, if one was found */
    size_t region;                     /* bytes reserved for it              */
    int occurrences;                   /* how many times it was found        */
    char uri[LIB_URI_MAX];             /* the URI as found, padding excluded */
    char base[LIB_URI_MAX];            /* a patched URI without the tab code */
    char code[TAB_CODE_MAX_LEN + 1];   /* the tab code a patched URI carries */
} lib_image;

/* Path of the game's main library. Caller frees. */
char *lib_path(const npp_paths *paths);

/*
 * Reads the library and works out which URI it carries. `known` lists the 3rd
 * party URIs to recognise (see server_known_uris). Returns 0 on success.
 */
int lib_open(const npp_paths *paths, const str_list *known, lib_image *img,
             char *err, size_t errsz);

void lib_close(lib_image *img);

/*
 * Writes `uri` over the URI region, padded with NUL bytes. Fails if it does
 * not fit. This is the only function here that changes the file.
 */
int lib_write_uri(const lib_image *img, const char *uri, char *err, size_t errsz);

/*
 * An author as the credit can carry it: the game's font has nothing outside
 * ASCII, so everything else is dropped — which is what the older installers
 * did — and what is left is trimmed to the budget. NULL when nothing usable
 * remains. Caller frees.
 */
char *lib_credit_text(const char *authors);

/* Whether the credit still reads what the game shipped with. */
int lib_credit_is_original(const lib_image *img);

/*
 * Overwrites the credit with `replacement`, NUL-padded. The string can only be
 * found by what it says, so `current` is what it is expected to say now: the
 * original when a tab is going in, the tab's author when one is coming out.
 * Returns 0, or -1 with a reason when the credit is not there, is there more
 * than once, or cannot be written. None of those stops the game working, so
 * callers report them rather than fail on them.
 */
int lib_write_credit(lib_image *img, const char *current, const char *replacement,
                     char *err, size_t errsz);

/*
 * Builds the URI to patch in: the server, then "/" and the tab code. Keeps the
 * scheme when there is room, drops it when there is not and the server speaks
 * HTTP. Returns NULL when even the shortest form is too long.
 */
char *lib_build_uri(const server_addr *addr, const char *code, size_t budget,
                    char *err, size_t errsz);

/* ---- Health check ------------------------------------------------------ */

typedef struct {
    int healthy;
    int unrecorded;                    /* a tab is in that we did not put in */
    lib_state state;                   /* what the library carries          */
    char uri[LIB_URI_MAX];             /* the URI found, if any             */
    char lib_code[TAB_CODE_MAX_LEN + 1];   /* tab the library points at     */
    char state_code[TAB_CODE_MAX_LEN + 1]; /* tab config.json says is in    */
    char detail[TB_ERR_LEN];           /* what is wrong, or what is unusual */
} lib_health;

/*
 * Compares what the library points at with what config.json says is installed,
 * and records the verdict under the "state" key. Returns 0 when the check could
 * be carried out (`out->healthy` says whether it passed), -1 when the library
 * could not even be read.
 */
int lib_check(config *cfg, const digest *dig, const npp_paths *paths,
              lib_health *out, char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_PATCH_H */
