/*
 * server.h - Which 3rd party server the game should be pointed at.
 *
 * The address is looked up in four places, in order:
 *
 *   1. the "server" object in config.json, so users can pick their own
 *   2. the "server" object in the digest, so the catalogue can move it
 *   3. the built-in host, outte.ovh
 *   4. the built-in address, used when the built-in host no longer resolves
 *
 * A server is {scheme, host, port}. "scheme" is optional: the game falls back
 * to plain HTTP when a URI carries no scheme, which is what lets us drop the
 * "http://" prefix when the patched URI would otherwise be too long.
 */
#ifndef TABBER_SERVER_H
#define TABBER_SERVER_H

#include "config.h"
#include "digest.h"
#include "json.h"
#include "util.h"

/* The 3rd party server, and the address to fall back on if its name expires. */
#define SERVER_DEFAULT_HOST     "outte.ovh"
#define SERVER_DEFAULT_PORT     8126
#define SERVER_FALLBACK_HOST    "45.32.150.168"
#define SERVER_FALLBACK_PORT    8126

/* "server" object keys, identical in config.json and in the digest. */
#define SJK_SERVER              "server"
#define SJK_SCHEME              "scheme"
#define SJK_HOST                "host"
#define SJK_PORT                "port"

#define SCHEME_HTTP             "http"
#define SCHEME_HTTPS            "https"
#define SCHEME_MARK             "://"

#define SERVER_SCHEME_MAX       8
#define SERVER_HOST_MAX         128

typedef struct {
    char scheme[SERVER_SCHEME_MAX];   /* empty means "unspecified", i.e. HTTP */
    char host[SERVER_HOST_MAX];
    int port;                         /* 0 when unspecified */
} server_addr;

/* Where a resolved address came from, for reporting. */
typedef enum {
    SERVER_FROM_CONFIG,
    SERVER_FROM_DIGEST,
    SERVER_FROM_DEFAULT,
    SERVER_FROM_FALLBACK
} server_source;

const char *server_source_name(server_source source);

/* Reads `parent`'s "server" object. Returns 1 when it names a usable host. */
int server_from_json(const json_value *parent, server_addr *out);

/*
 * Picks the server to patch in. `allow_lookup` permits a name resolution check
 * before settling for the built-in host: when it does not resolve, the built-in
 * address is used instead.
 */
void server_resolve(config *cfg, const digest *dig, int allow_lookup,
                    server_addr *out, server_source *source);

/* "scheme://host:port", or "host:port" without the scheme. Caller frees. */
char *server_uri(const server_addr *addr, int with_scheme);

/*
 * Every URI a patched library might carry, from all four sources, with and
 * without the scheme where the game would accept both. Used to recognise a
 * patched library without knowing which form was written.
 */
void server_known_uris(config *cfg, const digest *dig, str_list *out);

#endif /* TABBER_SERVER_H */
