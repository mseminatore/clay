/*
 * http.c - HTTP client prototype for Clay runtime
 *
 * Platform-specific HTTP implementations:
 * - Windows: WinHTTP (winhttp.h)
 * - Unix/macOS: POSIX sockets with manual HTTP/1.1
 */
#include "clay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static ClayValue http_get(ClayObject *self, int argc, ClayValue *argv);
static ClayValue http_post(ClayObject *self, int argc, ClayValue *argv);

/* Build a response object with status, body, headers */
static ClayValue make_response(int status_code, const char *body, size_t body_len) {
    extern ClayObject *clay_Http;
    ClayObject *resp = clay_clone(clay_Http);
    clay_set_slot(resp, "_name", CLAY_VAL(clay_string_new("HttpResponse")));
    clay_set_slot(resp, "_status", clay_int(status_code));
    clay_set_slot(resp, "_body", CLAY_VAL(clay_string_new_len(body, body_len)));
    return CLAY_VAL(resp);
}

static ClayValue make_error_response(const char *msg) {
    extern ClayObject *clay_Http;
    ClayObject *resp = clay_clone(clay_Http);
    clay_set_slot(resp, "_name", CLAY_VAL(clay_string_new("HttpResponse")));
    clay_set_slot(resp, "_status", clay_int(0));
    clay_set_slot(resp, "_body", CLAY_VAL(clay_string_new(msg)));
    return CLAY_VAL(resp);
}

/* Response accessor methods (registered on Http prototype, inherited by responses) */
static ClayValue resp_status(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_status");
}

static ClayValue resp_body(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_body");
}

/* --- Platform HTTP implementations --- */

#ifdef _WIN32
/* Windows: WinHTTP */

/* Avoid pulling in full windows.h — declare minimal types */
#ifndef _WINDEF_
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *LPVOID;
typedef const char *LPCSTR;
typedef unsigned short WORD;
typedef void *HINTERNET;
#endif

/* WinHTTP function declarations */
__declspec(dllimport) HINTERNET __stdcall WinHttpOpen(const wchar_t *, DWORD, const wchar_t *, const wchar_t *, DWORD);
__declspec(dllimport) HINTERNET __stdcall WinHttpConnect(HINTERNET, const wchar_t *, WORD, DWORD);
__declspec(dllimport) HINTERNET __stdcall WinHttpOpenRequest(HINTERNET, const wchar_t *, const wchar_t *, const wchar_t *, const wchar_t *, const wchar_t **, DWORD);
__declspec(dllimport) BOOL __stdcall WinHttpSendRequest(HINTERNET, const wchar_t *, DWORD, LPVOID, DWORD, DWORD, DWORD);
__declspec(dllimport) BOOL __stdcall WinHttpReceiveResponse(HINTERNET, LPVOID);
__declspec(dllimport) BOOL __stdcall WinHttpQueryDataAvailable(HINTERNET, DWORD *);
__declspec(dllimport) BOOL __stdcall WinHttpReadData(HINTERNET, LPVOID, DWORD, DWORD *);
__declspec(dllimport) BOOL __stdcall WinHttpQueryHeaders(HINTERNET, DWORD, const wchar_t *, LPVOID, DWORD *, DWORD *);
__declspec(dllimport) BOOL __stdcall WinHttpCloseHandle(HINTERNET);
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int, DWORD, LPCSTR, int, wchar_t *, int);

#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
#define WINHTTP_NO_PROXY_NAME NULL
#define WINHTTP_NO_PROXY_BYPASS NULL
#define WINHTTP_NO_REFERER NULL
#define WINHTTP_DEFAULT_ACCEPT_TYPES NULL
#define WINHTTP_NO_ADDITIONAL_HEADERS NULL
#define WINHTTP_FLAG_SECURE 0x00800000
#define WINHTTP_QUERY_STATUS_CODE 19
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000
#define INTERNET_DEFAULT_HTTP_PORT 80
#define INTERNET_DEFAULT_HTTPS_PORT 443

/* Helper: convert UTF-8 to wide string */
static wchar_t *to_wide(const char *s) {
    int len = MultiByteToWideChar(65001, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    MultiByteToWideChar(65001, 0, s, -1, w, len);
    return w;
}

/* Parse URL into host, path, port, and https flag */
static int parse_url(const char *url, char *host, size_t host_sz,
                     char *path, size_t path_sz, int *port, int *is_https) {
    const char *p = url;
    *is_https = 0;
    if (strncmp(p, "https://", 8) == 0) { *is_https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, p, hlen); host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, p, hlen); host[hlen] = '\0';
        *port = *is_https ? 443 : 80;
    }
    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= path_sz) plen = path_sz - 1;
        memcpy(path, slash, plen); path[plen] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

static ClayValue do_http_request(const char *url, const char *method, const char *req_body, size_t body_len) {
    char host[256], path[1024];
    int port, is_https;
    parse_url(url, host, sizeof(host), path, sizeof(path), &port, &is_https);

    wchar_t *w_host = to_wide(host);
    wchar_t *w_path = to_wide(path);
    wchar_t *w_method = to_wide(method);

    HINTERNET session = WinHttpOpen(L"Clay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { free(w_host); free(w_path); free(w_method); return make_error_response("WinHttpOpen failed"); }

    HINTERNET conn = WinHttpConnect(session, w_host, (WORD)port, 0);
    if (!conn) { WinHttpCloseHandle(session); free(w_host); free(w_path); free(w_method); return make_error_response("WinHttpConnect failed"); }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, w_method, w_path, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    free(w_host); free(w_path); free(w_method);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return make_error_response("WinHttpOpenRequest failed"); }

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  (LPVOID)req_body, (DWORD)body_len, (DWORD)body_len, 0);
    if (!ok) { WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return make_error_response("WinHttpSendRequest failed"); }

    ok = WinHttpReceiveResponse(req, NULL);
    if (!ok) { WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return make_error_response("WinHttpReceiveResponse failed"); }

    /* Get status code */
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    DWORD index = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         NULL, &status_code, &status_size, &index);

    /* Read body */
    char *buf = NULL;
    size_t total = 0;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        buf = (char *)realloc(buf, total + avail + 1);
        DWORD read = 0;
        WinHttpReadData(req, buf + total, avail, &read);
        total += read;
    }
    if (buf) buf[total] = '\0';

    ClayValue result = make_response((int)status_code, buf ? buf : "", total);

    free(buf);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

#else
/* Unix/macOS: POSIX sockets */
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

static ClayValue do_http_request(const char *url, const char *method, const char *req_body, size_t body_len) {
    /* Parse URL */
    const char *p = url;
    int is_https = 0;
    if (strncmp(p, "https://", 8) == 0) { is_https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }

    if (is_https) return make_error_response("HTTPS not supported on Unix (no TLS)");

    char host[256] = {0};
    char path[1024] = "/";
    char port_str[8] = "80";

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
        const char *port_start = colon + 1;
        size_t plen = slash ? (size_t)(slash - port_start) : strlen(port_start);
        if (plen >= sizeof(port_str)) plen = sizeof(port_str) - 1;
        memcpy(port_str, port_start, plen); port_str[plen] = '\0';
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
    }
    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, slash, plen); path[plen] = '\0';
    }

    /* Connect */
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return make_error_response("DNS lookup failed");

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return make_error_response("socket failed"); }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res); close(fd); return make_error_response("connect failed");
    }
    freeaddrinfo(res);

    /* Build HTTP request */
    char header[2048];
    if (req_body && body_len > 0) {
        snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            method, path, host, body_len);
    } else {
        snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n\r\n",
            method, path, host);
    }

    send(fd, header, strlen(header), 0);
    if (req_body && body_len > 0)
        send(fd, req_body, body_len, 0);

    /* Read response */
    char *buf = NULL;
    size_t total = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        buf = (char *)realloc(buf, total + (size_t)n + 1);
        memcpy(buf + total, chunk, (size_t)n);
        total += (size_t)n;
    }
    close(fd);
    if (buf) buf[total] = '\0';

    /* Parse status line */
    int status_code = 0;
    if (buf && strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf, ' ');
        if (sp) status_code = atoi(sp + 1);
    }

    /* Find body (after \r\n\r\n) */
    const char *body_start = buf ? strstr(buf, "\r\n\r\n") : NULL;
    size_t body_offset = 0;
    if (body_start) body_offset = (size_t)(body_start + 4 - buf);

    ClayValue result = make_response(status_code,
        buf ? buf + body_offset : "", total > body_offset ? total - body_offset : 0);

    free(buf);
    return result;
}
#endif /* _WIN32 */

/* Http get(url) */
static ClayValue http_get(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc;
    const char *url = clay_string_data(CLAY_OBJ(argv[0]));
    return do_http_request(url, "GET", NULL, 0);
}

/* Http post(url, body) */
static ClayValue http_post(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc;
    const char *url = clay_string_data(CLAY_OBJ(argv[0]));
    const char *body = argc > 1 ? clay_string_data(CLAY_OBJ(argv[1])) : "";
    size_t body_len = argc > 1 ? strlen(body) : 0;
    return do_http_request(url, "POST", body, body_len);
}

/* Register Http methods */
void clay_http_init(ClayObject *http) {
    clay_set_method(http, "get", http_get);
    clay_set_method(http, "post", http_post);
    clay_set_method(http, "status", resp_status);
    clay_set_method(http, "body", resp_body);
}
