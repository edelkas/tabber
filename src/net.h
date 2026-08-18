/*
 * net.h - Minimal HTTPS client.
 *
 * Backed by WinHTTP on Windows (no external dependency) and by libcurl
 * elsewhere. Redirects are followed, which the GitHub download links rely on.
 */
#ifndef TABBER_NET_H
#define TABBER_NET_H

#include <stddef.h>

#include "version.h"

/* User agent sent with every request. */
#define NET_USER_AGENT      TABBER_NAME "/" TABBER_VERSION

/* Give up on a stalled transfer after this many seconds. */
#define NET_TIMEOUT_SECS    30

/* Probes are diagnostics: a server that is up answers one straight away. */
#define NET_PROBE_TIMEOUT_SECS  5

/* Refuse absurdly large replies; the digest is a few tens of KB. */
#define NET_MAX_RESPONSE    (256u * 1024u * 1024u)

/*
 * Downloads `url` into a freshly allocated, NUL-terminated buffer (the
 * terminator is not counted in *len_out). Returns 0 on success, -1 on failure
 * with a reason in `err`. The caller frees *data.
 */
int net_fetch(const char *url, char **data, size_t *len_out, char *err, size_t errsz);

/*
 * Asks `url` for its status code and throws the body away. Returns 0 and sets
 * *status when the server answered anything at all — any status proves it is
 * listening — or -1 with a reason in `err` when the exchange never got that
 * far. `timeout_secs` keeps a probe from stalling a command.
 */
int net_probe(const char *url, int timeout_secs, int *status, char *err, size_t errsz);

/*
 * Whether a host name resolves. Used to decide if the built-in server name is
 * still alive before patching it into the game. An address literal always
 * "resolves", and a machine with no network at all reports failure.
 */
int net_host_resolves(const char *host);

#endif /* TABBER_NET_H */
