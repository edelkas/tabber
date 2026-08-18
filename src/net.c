#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "platform.h"
#include "util.h"

/* ====================================================================== */
/*  Windows: WinHTTP                                                      */
/* ====================================================================== */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <ws2tcpip.h>   /* getaddrinfo */

#ifdef _MSC_VER
#  pragma comment(lib, "winhttp.lib")
#  pragma comment(lib, "ws2_32.lib")
#endif

/* Size of the chunks pulled out of a response body. */
#define WIN_READ_CHUNK 16384

/* Converts UTF-8 to UTF-16 (local copy: net.c is the only other Win32 user). */
static wchar_t *net_wide(const char *s)
{
    int chars;
    wchar_t *out;

    if (!s)
        return NULL;
    chars = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (chars <= 0)
        return NULL;
    out = xmalloc((size_t)chars * sizeof(wchar_t));
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, chars) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

/*
 * Turns the most common WinHTTP failures into something a user can act on.
 * Returns NULL for codes we have no wording for.
 */
static const char *net_win_error_text(DWORD code)
{
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:             return "the connection timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:   return "the server name could not be resolved, check your connection";
        case ERROR_WINHTTP_CANNOT_CONNECT:      return "the server refused the connection";
        case ERROR_WINHTTP_CONNECTION_ERROR:    return "the connection was lost";
        case ERROR_WINHTTP_SECURE_FAILURE:      return "the secure connection failed (certificate problem?)";
        case ERROR_WINHTTP_INVALID_URL:         return "the URL is malformed";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "the URL scheme is not supported";
        default:                                return NULL;
    }
}

/* Reports a Win32 failure, preferring a readable description when we have one. */
static void net_win_fail(char *err, size_t errsz, const char *context, DWORD code)
{
    const char *text = net_win_error_text(code);

    if (text)
        err_set(err, errsz, "%s: %s", context, text);
    else
        err_set(err, errsz, "%s (WinHTTP error %lu)", context, (unsigned long)code);
}

/* Copies a (non NUL-terminated) URL component out of URL_COMPONENTS. */
static wchar_t *net_wide_dup(const wchar_t *start, DWORD len)
{
    wchar_t *out = xmalloc(((size_t)len + 1) * sizeof(wchar_t));
    memcpy(out, start, (size_t)len * sizeof(wchar_t));
    out[len] = L'\0';
    return out;
}

/* An open WinHTTP conversation, torn down in one go by net_win_close. */
typedef struct {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
} win_request;

static void net_win_close(win_request *req)
{
    if (req->request)    WinHttpCloseHandle(req->request);
    if (req->connection) WinHttpCloseHandle(req->connection);
    if (req->session)    WinHttpCloseHandle(req->session);
    memset(req, 0, sizeof(*req));
}

/*
 * Sends a GET and reads the response status. On success the response is left
 * open so the body can be read; on failure the handles are already closed,
 * though closing them again is harmless.
 */
static int net_win_send(const char *url, int timeout_secs, win_request *req,
                        DWORD *status, char *err, size_t errsz)
{
    wchar_t *wurl = NULL, *whost = NULL, *wpath = NULL, *wagent = NULL;
    URL_COMPONENTS parts;
    DWORD status_size = sizeof(*status);
    int rc = -1;

    memset(req, 0, sizeof(*req));
    *status = 0;

    wurl = net_wide(url);
    if (!wurl) {
        err_set(err, errsz, "invalid URL");
        goto done;
    }

    /* Split the URL: WinHttpConnect wants the host, OpenRequest the path. */
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        err_set(err, errsz, "cannot parse URL '%s'", url);
        goto done;
    }
    whost = net_wide_dup(parts.lpszHostName, parts.dwHostNameLength);
    /* Path and query sit next to each other in the original string. */
    wpath = net_wide_dup(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);

    wagent = net_wide(NET_USER_AGENT);   /* WinHTTP is a wide-character API */
    req->session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!req->session) {
        net_win_fail(err, errsz, "cannot initialise WinHTTP", GetLastError());
        goto done;
    }
    {
        int ms = timeout_secs * 1000;
        WinHttpSetTimeouts(req->session, ms, ms, ms, ms);
    }

    req->connection = WinHttpConnect(req->session, whost, parts.nPort, 0);
    if (!req->connection) {
        net_win_fail(err, errsz, "cannot connect to the server", GetLastError());
        goto done;
    }

    req->request = WinHttpOpenRequest(req->connection, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req->request) {
        net_win_fail(err, errsz, "cannot create the request", GetLastError());
        goto done;
    }

    if (!WinHttpSendRequest(req->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req->request, NULL)) {
        net_win_fail(err, errsz, "request failed", GetLastError());
        goto done;
    }

    if (!WinHttpQueryHeaders(req->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        net_win_fail(err, errsz, "cannot read the response status", GetLastError());
        goto done;
    }
    rc = 0;

done:
    if (rc != 0)
        net_win_close(req);
    free(wurl);
    free(whost);
    free(wpath);
    free(wagent);
    return rc;
}

int net_fetch(const char *url, char **data, size_t *len_out, char *err, size_t errsz)
{
    win_request req;
    DWORD status = 0;
    byte_buf body = {0};
    int rc = -1;

    *data = NULL;
    if (len_out)
        *len_out = 0;

    if (net_win_send(url, NET_TIMEOUT_SECS, &req, &status, err, errsz) != 0)
        return -1;

    if (status != 200) {
        err_set(err, errsz, "server replied with HTTP %lu", (unsigned long)status);
        goto done;
    }

    for (;;) {
        char chunk[WIN_READ_CHUNK];
        DWORD got = 0;

        if (!WinHttpReadData(req.request, chunk, sizeof(chunk), &got)) {
            net_win_fail(err, errsz, "download interrupted", GetLastError());
            goto done;
        }
        if (got == 0)
            break;
        buf_append(&body, chunk, got);
        if (body.len > NET_MAX_RESPONSE) {
            err_set(err, errsz, "response larger than %u bytes", NET_MAX_RESPONSE);
            goto done;
        }
    }

    *data = buf_finish(&body, len_out);
    rc = 0;

done:
    buf_free(&body);
    net_win_close(&req);
    return rc;
}

int net_probe(const char *url, int timeout_secs, int *status, char *err, size_t errsz)
{
    win_request req;
    DWORD code = 0;

    if (status)
        *status = 0;
    if (net_win_send(url, timeout_secs, &req, &code, err, errsz) != 0)
        return -1;

    /* The body is of no interest: an answer of any kind is the whole point. */
    net_win_close(&req);
    if (status)
        *status = (int)code;
    return 0;
}

int net_host_resolves(const char *host)
{
    WSADATA wsa;
    struct addrinfo hints, *result = NULL;
    int ok;

    if (!host || !*host)
        return 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    ok = getaddrinfo(host, NULL, &hints, &result) == 0;
    if (result)
        freeaddrinfo(result);
    WSACleanup();
    return ok;
}

/* ====================================================================== */
/*  POSIX: libcurl                                                        */
/* ====================================================================== */
#else

#include <curl/curl.h>
#include <netdb.h>      /* getaddrinfo */
#include <sys/socket.h>

int net_host_resolves(const char *host)
{
    struct addrinfo hints, *result = NULL;
    int ok;

    if (!host || !*host)
        return 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    ok = getaddrinfo(host, NULL, &hints, &result) == 0;
    if (result)
        freeaddrinfo(result);
    return ok;
}

/* Accumulates the response body, enforcing the size cap. */
static size_t net_sink(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    byte_buf *body = userdata;
    size_t len = size * nmemb;

    if (body->len + len > NET_MAX_RESPONSE)
        return 0;   /* signals an error to libcurl, aborting the transfer */
    buf_append(body, ptr, len);
    return len;
}

/* Throws the body away: a probe only cares about the status line. */
static size_t net_drain(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

/* A handle with the options both requests share. NULL when libcurl says no. */
static CURL *net_curl_open(const char *url, int timeout_secs)
{
    static int global_ready = 0;
    CURL *curl;

    if (!global_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        global_ready = 1;
    }

    curl = curl_easy_init();
    if (!curl)
        return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_secs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NET_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    return curl;
}

int net_fetch(const char *url, char **data, size_t *len_out, char *err, size_t errsz)
{
    CURL *curl;
    CURLcode res;
    long status = 0;
    byte_buf body = {0};

    *data = NULL;
    if (len_out)
        *len_out = 0;

    curl = net_curl_open(url, NET_TIMEOUT_SECS);
    if (!curl) {
        err_set(err, errsz, "cannot initialise libcurl");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, net_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* allow compression */

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        err_set(err, errsz, "%s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        buf_free(&body);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (status != 200) {
        err_set(err, errsz, "server replied with HTTP %ld", status);
        buf_free(&body);
        return -1;
    }

    *data = buf_finish(&body, len_out);
    return 0;
}

int net_probe(const char *url, int timeout_secs, int *status, char *err, size_t errsz)
{
    CURL *curl;
    CURLcode res;
    long code = 0;

    if (status)
        *status = 0;

    curl = net_curl_open(url, timeout_secs);
    if (!curl) {
        err_set(err, errsz, "cannot initialise libcurl");
        return -1;
    }

    /* No redirect following: a redirect is itself an answer, and chasing it
     * would end up asking a different server whether this one is up. */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, net_drain);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        err_set(err, errsz, "%s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (code == 0) {
        err_set(err, errsz, "the server did not reply with a status");
        return -1;
    }

    if (status)
        *status = (int)code;
    return 0;
}

#endif /* _WIN32 */
