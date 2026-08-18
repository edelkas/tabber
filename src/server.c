#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "server.h"
#include "util.h"

const char *server_source_name(server_source source)
{
    switch (source) {
        case SERVER_FROM_CONFIG:   return "config.json";
        case SERVER_FROM_DIGEST:   return "the digest";
        case SERVER_FROM_DEFAULT:  return "the built-in host";
        case SERVER_FROM_FALLBACK: return "the built-in address";
        default:                   return "?";
    }
}

/* Fills an address from its parts, normalising the scheme. */
static void server_set(server_addr *addr, const char *scheme, const char *host, int port)
{
    memset(addr, 0, sizeof(*addr));
    if (scheme && *scheme)
        snprintf(addr->scheme, sizeof addr->scheme, "%s", scheme);
    if (host)
        snprintf(addr->host, sizeof addr->host, "%s", host);
    addr->port = port;
}

int server_from_json(const json_value *parent, server_addr *out)
{
    const json_value *node = json_get(parent, SJK_SERVER);
    const char *scheme, *host;

    if (!node || node->type != JSON_OBJECT)
        return 0;

    host = json_get_string(node, SJK_HOST, NULL);
    if (!host || !*host)
        return 0;   /* a server without a host is unusable */

    scheme = json_get_string(node, SJK_SCHEME, NULL);
    if (scheme && !str_ieq(scheme, SCHEME_HTTP) && !str_ieq(scheme, SCHEME_HTTPS))
        scheme = NULL;   /* anything else is not something the game speaks */

    server_set(out, scheme, host, (int)json_get_int(node, SJK_PORT, 0));
    return 1;
}

void server_resolve(config *cfg, const digest *dig, int allow_lookup,
                    server_addr *out, server_source *source)
{
    if (cfg && server_from_json(cfg->root, out)) {
        *source = SERVER_FROM_CONFIG;
        return;
    }
    if (dig && server_from_json(dig->root, out)) {
        *source = SERVER_FROM_DIGEST;
        return;
    }

    /* Neither file names one: use the built-in host, unless its name has
     * stopped resolving, in which case go straight to the address. */
    server_set(out, NULL, SERVER_DEFAULT_HOST, SERVER_DEFAULT_PORT);
    *source = SERVER_FROM_DEFAULT;
    if (allow_lookup && !net_host_resolves(SERVER_DEFAULT_HOST)) {
        server_set(out, NULL, SERVER_FALLBACK_HOST, SERVER_FALLBACK_PORT);
        *source = SERVER_FROM_FALLBACK;
    }
}

char *server_uri(const server_addr *addr, int with_scheme)
{
    const char *scheme = addr->scheme[0] ? addr->scheme : SCHEME_HTTP;
    char port[16];

    port[0] = '\0';
    if (addr->port > 0)
        snprintf(port, sizeof port, ":%d", addr->port);

    if (with_scheme)
        return str_fmt("%s%s%s%s", scheme, SCHEME_MARK, addr->host, port);
    return str_fmt("%s%s", addr->host, port);
}

/* Adds an address's possible URI forms, skipping duplicates. */
static void push_forms(str_list *out, const server_addr *addr)
{
    char *with = server_uri(addr, 1);
    char *without = server_uri(addr, 0);

    if (!str_list_contains(out, with))
        str_list_push(out, with);
    else
        free(with);

    /*
     * Only HTTP servers may appear without a scheme: dropping "https://" would
     * change which protocol the game speaks, so no patcher would write that.
     */
    if (!addr->scheme[0] || str_ieq(addr->scheme, SCHEME_HTTP)) {
        if (!str_list_contains(out, without))
            str_list_push(out, without);
        else
            free(without);
    } else {
        free(without);
    }
}

void server_known_uris(config *cfg, const digest *dig, str_list *out)
{
    server_addr addr;

    if (cfg && server_from_json(cfg->root, &addr))
        push_forms(out, &addr);
    if (dig && server_from_json(dig->root, &addr))
        push_forms(out, &addr);

    server_set(&addr, NULL, SERVER_DEFAULT_HOST, SERVER_DEFAULT_PORT);
    push_forms(out, &addr);
    server_set(&addr, NULL, SERVER_FALLBACK_HOST, SERVER_FALLBACK_PORT);
    push_forms(out, &addr);
}
