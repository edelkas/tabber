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

/* Refuse absurdly large replies; the digest is a few tens of KB. */
#define NET_MAX_RESPONSE    (256u * 1024u * 1024u)

/*
 * Downloads `url` into a freshly allocated, NUL-terminated buffer (the
 * terminator is not counted in *len_out). Returns 0 on success, -1 on failure
 * with a reason in `err`. The caller frees *data.
 */
int net_fetch(const char *url, char **data, size_t *len_out, char *err, size_t errsz);

#endif /* TABBER_NET_H */
