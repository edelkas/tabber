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

#ifdef _MSC_VER
#  pragma comment(lib, "winhttp.lib")
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

int net_fetch(const char *url, char **data, size_t *len_out, char *err, size_t errsz)
{
    wchar_t *wurl = NULL, *whost = NULL, *wpath = NULL, *wagent = NULL;
    HINTERNET session = NULL, connection = NULL, request = NULL;
    URL_COMPONENTS parts;
    DWORD status = 0, status_size = sizeof(status);
    byte_buf body = {0};
    int rc = -1;

    *data = NULL;
    if (len_out)
        *len_out = 0;

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
    session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        net_win_fail(err, errsz, "cannot initialise WinHTTP", GetLastError());
        goto done;
    }
    {
        int ms = NET_TIMEOUT_SECS * 1000;
        WinHttpSetTimeouts(session, ms, ms, ms, ms);
    }

    connection = WinHttpConnect(session, whost, parts.nPort, 0);
    if (!connection) {
        net_win_fail(err, errsz, "cannot connect to the server", GetLastError());
        goto done;
    }

    request = WinHttpOpenRequest(connection, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        net_win_fail(err, errsz, "cannot create the request", GetLastError());
        goto done;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        net_win_fail(err, errsz, "request failed", GetLastError());
        goto done;
    }

    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        net_win_fail(err, errsz, "cannot read the response status", GetLastError());
        goto done;
    }
    if (status != 200) {
        err_set(err, errsz, "server replied with HTTP %lu", (unsigned long)status);
        goto done;
    }

    for (;;) {
        char chunk[WIN_READ_CHUNK];
        DWORD got = 0;

        if (!WinHttpReadData(request, chunk, sizeof(chunk), &got)) {
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
    if (request)    WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session)    WinHttpCloseHandle(session);
    free(wurl);
    free(whost);
    free(wpath);
    free(wagent);
    return rc;
}

/* ====================================================================== */
/*  POSIX: libcurl                                                        */
/* ====================================================================== */
#else

#include <curl/curl.h>

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

int net_fetch(const char *url, char **data, size_t *len_out, char *err, size_t errsz)
{
    static int global_ready = 0;
    CURL *curl;
    CURLcode res;
    long status = 0;
    byte_buf body = {0};

    *data = NULL;
    if (len_out)
        *len_out = 0;

    if (!global_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        global_ready = 1;
    }

    curl = curl_easy_init();
    if (!curl) {
        err_set(err, errsz, "cannot initialise libcurl");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, net_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)NET_TIMEOUT_SECS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NET_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* allow compression */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

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

#endif /* _WIN32 */
