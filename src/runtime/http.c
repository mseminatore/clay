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

#ifdef CLAY_HAS_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

static ClayValue do_http_request(const char *url, const char *method, const char *req_body, size_t body_len) {
    /* Parse URL */
    const char *p = url;
    int is_https = 0;
    if (strncmp(p, "https://", 8) == 0) { is_https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }

#ifndef CLAY_HAS_SSL
    if (is_https) return make_error_response("HTTPS not supported (build without OpenSSL)");
#endif

    char host[256] = {0};
    char path[1024] = "/";
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", is_https ? 443 : 80);

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

    /* Send and receive — with optional TLS */
#ifdef CLAY_HAS_SSL
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    if (is_https) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) { close(fd); return make_error_response("SSL_CTX_new failed"); }
        ssl = SSL_new(ssl_ctx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl); SSL_CTX_free(ssl_ctx); close(fd);
            return make_error_response("SSL handshake failed");
        }
    }
#define HTTP_SEND(buf, len) (is_https ? SSL_write(ssl, buf, (int)(len)) : send(fd, buf, len, 0))
#define HTTP_RECV(buf, len) (is_https ? SSL_read(ssl, buf, (int)(len)) : (int)recv(fd, buf, len, 0))
#else
#define HTTP_SEND(buf, len) send(fd, buf, len, 0)
#define HTTP_RECV(buf, len) (int)recv(fd, buf, len, 0)
#endif

    HTTP_SEND(header, strlen(header));
    if (req_body && body_len > 0)
        HTTP_SEND(req_body, body_len);

    /* Read response */
    char *buf = NULL;
    size_t total = 0;
    char chunk[4096];
    int n;
    while ((n = HTTP_RECV(chunk, sizeof(chunk))) > 0) {
        buf = (char *)realloc(buf, total + (size_t)n + 1);
        memcpy(buf + total, chunk, (size_t)n);
        total += (size_t)n;
    }

#ifdef CLAY_HAS_SSL
    if (is_https) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
    close(fd);

#undef HTTP_SEND
#undef HTTP_RECV
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

/* --- HTTP Server --- */

/* Build a request object for the handler */
static ClayValue make_request(const char *method_str, const char *path,
                               const char *query, const char *body_data,
                               size_t body_sz) {
    extern ClayObject *clay_Http;
    ClayObject *req = clay_clone(clay_Http);
    clay_set_slot(req, "_name", CLAY_VAL(clay_string_new("HttpRequest")));
    clay_set_slot(req, "_method", CLAY_VAL(clay_string_new(method_str)));
    clay_set_slot(req, "_path", CLAY_VAL(clay_string_new(path)));
    clay_set_slot(req, "_query", CLAY_VAL(clay_string_new(query ? query : "")));
    clay_set_slot(req, "_body",
        CLAY_VAL(body_data ? clay_string_new_len(body_data, body_sz)
                           : clay_string_new("")));
    return CLAY_VAL(req);
}

/* Request accessor methods */
static ClayValue req_method(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_method");
}
static ClayValue req_path(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_path");
}
static ClayValue req_query(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_query");
}
static ClayValue req_params(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue v = clay_get_slot(self, "_params");
    if (v == CLAY_NIL) return CLAY_VAL(clay_dict_new());
    return v;
}

/* Http response(status, body) — create a response object */
static ClayValue http_response(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_NIL;
    int status_code = CLAY_IS_INT(argv[0]) ? (int)clay_to_int(argv[0]) : 200;
    const char *body = clay_string_data(CLAY_OBJ(argv[1]));
    size_t blen = strlen(body);
    ClayValue resp = make_response(status_code, body, blen);
    /* Auto-detect HTML content */
    if (blen > 0 && body[0] == '<')
        clay_set_slot(CLAY_OBJ(resp), "_content_type",
            CLAY_VAL(clay_string_new("text/html")));
    return resp;
}

/* Http jsonResponse(status, body) — response with JSON content type */
static ClayValue http_json_response(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_NIL;
    int status_code = CLAY_IS_INT(argv[0]) ? (int)clay_to_int(argv[0]) : 200;
    const char *body = clay_string_data(CLAY_OBJ(argv[1]));
    size_t blen = strlen(body);
    extern ClayObject *clay_Http;
    ClayObject *resp = clay_clone(clay_Http);
    clay_set_slot(resp, "_name", CLAY_VAL(clay_string_new("HttpResponse")));
    clay_set_slot(resp, "_status", clay_int(status_code));
    clay_set_slot(resp, "_body", CLAY_VAL(clay_string_new_len(body, blen)));
    clay_set_slot(resp, "_content_type",
        CLAY_VAL(clay_string_new("application/json")));
    return CLAY_VAL(resp);
}

/* Parse an HTTP request from raw data. Modifies buf in-place (adds NULs). */
static void parse_http_request(char *buf, size_t len,
                                char **method_out, char **path_out,
                                char **query_out, char **body_out,
                                size_t *body_len_out) {
    *method_out = buf;
    *path_out = "/";
    *query_out = NULL;
    *body_out = NULL;
    *body_len_out = 0;

    /* Find end of method */
    char *sp = strchr(buf, ' ');
    if (!sp) return;
    *sp = '\0';
    char *uri = sp + 1;

    /* Find end of URI */
    sp = strchr(uri, ' ');
    if (sp) *sp = '\0';

    /* Split URI into path and query */
    char *qmark = strchr(uri, '?');
    if (qmark) {
        *qmark = '\0';
        *query_out = qmark + 1;
    }
    *path_out = uri;

    /* Find body (after \r\n\r\n) */
    char *body_sep = strstr(sp ? sp + 1 : uri, "\r\n\r\n");
    if (body_sep) {
        *body_out = body_sep + 4;
        *body_len_out = len - (size_t)(*body_out - buf);
    }
}

/* Platform-specific server implementation */

static const char *http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

#ifdef _WIN32

/* Winsock2 declarations — minimal, avoiding winsock2.h include */
typedef unsigned int UINT_PTR;
typedef UINT_PTR SOCKET;
#define INVALID_SOCKET ((SOCKET)(~0))
#define SOCKET_ERROR (-1)
#define SD_SEND 1
#define AF_INET2 2
#define SOCK_STREAM2 1

struct ws_sockaddr_in {
    short sin_family;
    unsigned short sin_port;
    struct { unsigned long s_addr; } sin_addr;
    char sin_zero[8];
};

struct ws_wsadata { WORD wVersion; WORD wHighVersion; char szDescription[257]; char szSystemStatus[129]; unsigned short iMaxSockets; unsigned short iMaxUdpDg; char *lpVendorInfo; };

__declspec(dllimport) int __stdcall WSAStartup(WORD, struct ws_wsadata *);
__declspec(dllimport) int __stdcall WSACleanup(void);
__declspec(dllimport) SOCKET __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall bind(SOCKET, const void *, int);
__declspec(dllimport) int __stdcall listen(SOCKET, int);
__declspec(dllimport) SOCKET __stdcall accept(SOCKET, void *, int *);
__declspec(dllimport) int __stdcall recv(SOCKET, char *, int, int);
__declspec(dllimport) int __stdcall send(SOCKET, const char *, int, int);
__declspec(dllimport) int __stdcall closesocket(SOCKET);
__declspec(dllimport) int __stdcall shutdown(SOCKET, int);
__declspec(dllimport) int __stdcall setsockopt(SOCKET, int, int, const char *, int);
__declspec(dllimport) unsigned short __stdcall htons(unsigned short);

static ClayValue http_serve(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_NIL;
    int port = CLAY_IS_INT(argv[0]) ? (int)clay_to_int(argv[0]) : 8080;
    ClayValue handler = argv[1];

    struct ws_wsadata wsa;
    WSAStartup(0x0202, &wsa);

    SOCKET server_fd = socket(AF_INET2, SOCK_STREAM2, 0);
    if (server_fd == INVALID_SOCKET) {
        fprintf(stderr, "Http serve: socket creation failed\n");
        return CLAY_NIL;
    }

    int opt = 1;
    setsockopt(server_fd, 0xFFFF, 0x0004, (const char *)&opt, sizeof(opt)); /* SOL_SOCKET, SO_REUSEADDR */

    struct ws_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET2;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = 0; /* INADDR_ANY */

    if (bind(server_fd, &addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Http serve: bind to port %d failed\n", port);
        closesocket(server_fd);
        return CLAY_NIL;
    }
    if (listen(server_fd, 16) == SOCKET_ERROR) {
        fprintf(stderr, "Http serve: listen failed\n");
        closesocket(server_fd);
        return CLAY_NIL;
    }

    printf("Clay HTTP server listening on port %d\n", port);

    for (;;) {
        struct ws_sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_fd = accept(server_fd, &client_addr, &client_len);
        if (client_fd == INVALID_SOCKET) continue;

        /* Read request */
        char buf[8192];
        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { closesocket(client_fd); continue; }
        buf[n] = '\0';

        char *method_str, *path, *query, *body_data;
        size_t body_sz;
        parse_http_request(buf, (size_t)n, &method_str, &path, &query, &body_data, &body_sz);

        ClayValue req = make_request(method_str, path, query, body_data, body_sz);

        /* Call handler */
        ClayValue resp = clay_send(CLAY_OBJ(handler), "value", 1, &req);

        /* Build HTTP response */
        const char *resp_body = "";
        int resp_status = 200;
        const char *content_type = "text/plain";
        const char *location = NULL;
        if (resp != CLAY_NIL && CLAY_IS_PTR(resp)) {
            ClayValue sv = clay_get_slot(CLAY_OBJ(resp), "_status");
            if (CLAY_IS_INT(sv)) resp_status = (int)clay_to_int(sv);
            ClayValue bv = clay_get_slot(CLAY_OBJ(resp), "_body");
            if (bv != CLAY_NIL && CLAY_IS_PTR(bv)) resp_body = clay_string_data(CLAY_OBJ(bv));
            ClayValue ct = clay_get_slot(CLAY_OBJ(resp), "_content_type");
            if (ct != CLAY_NIL && CLAY_IS_PTR(ct)) content_type = clay_string_data(CLAY_OBJ(ct));
            ClayValue loc = clay_get_slot(CLAY_OBJ(resp), "_location");
            if (loc != CLAY_NIL && CLAY_IS_PTR(loc)) location = clay_string_data(CLAY_OBJ(loc));
        }

        char http_header[512];
        if (location) {
            snprintf(http_header, sizeof(http_header),
                "HTTP/1.1 %d %s\r\nLocation: %s\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n",
                resp_status, http_status_text(resp_status), location);
        } else {
            snprintf(http_header, sizeof(http_header),
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                resp_status, http_status_text(resp_status), content_type, strlen(resp_body));
        }

        send(client_fd, http_header, (int)strlen(http_header), 0);
        if (!location)
            send(client_fd, resp_body, (int)strlen(resp_body), 0);
        shutdown(client_fd, SD_SEND);
        closesocket(client_fd);

        clay_release(CLAY_OBJ(req));
        if (resp != CLAY_NIL && CLAY_IS_PTR(resp)) clay_release(CLAY_OBJ(resp));
    }

    closesocket(server_fd);
    WSACleanup();
    return CLAY_NIL;
}

#else
/* Unix/macOS server */
#include <netinet/in.h>
#include <signal.h>

static ClayValue http_serve(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_NIL;
    int port = CLAY_IS_INT(argv[0]) ? (int)clay_to_int(argv[0]) : 8080;
    ClayValue handler = argv[1];

    /* Ignore SIGPIPE so we don't crash on broken connections */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Http serve: socket creation failed\n");
        return CLAY_NIL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Http serve: bind to port %d failed\n", port);
        close(server_fd);
        return CLAY_NIL;
    }
    if (listen(server_fd, 16) < 0) {
        fprintf(stderr, "Http serve: listen failed\n");
        close(server_fd);
        return CLAY_NIL;
    }

    printf("Clay HTTP server listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        /* Read request */
        char buf[8192];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(client_fd); continue; }
        buf[n] = '\0';

        char *method_str, *path, *query, *body_data;
        size_t body_sz;
        parse_http_request(buf, (size_t)n, &method_str, &path, &query, &body_data, &body_sz);

        ClayValue req = make_request(method_str, path, query, body_data, body_sz);

        /* Call handler */
        ClayValue resp = clay_send(CLAY_OBJ(handler), "value", 1, &req);

        /* Build HTTP response */
        const char *resp_body = "";
        int resp_status = 200;
        const char *content_type = "text/plain";
        const char *location = NULL;
        if (resp != CLAY_NIL && CLAY_IS_PTR(resp)) {
            ClayValue sv = clay_get_slot(CLAY_OBJ(resp), "_status");
            if (CLAY_IS_INT(sv)) resp_status = (int)clay_to_int(sv);
            ClayValue bv = clay_get_slot(CLAY_OBJ(resp), "_body");
            if (bv != CLAY_NIL && CLAY_IS_PTR(bv)) resp_body = clay_string_data(CLAY_OBJ(bv));
            ClayValue ct = clay_get_slot(CLAY_OBJ(resp), "_content_type");
            if (ct != CLAY_NIL && CLAY_IS_PTR(ct)) content_type = clay_string_data(CLAY_OBJ(ct));
            ClayValue loc = clay_get_slot(CLAY_OBJ(resp), "_location");
            if (loc != CLAY_NIL && CLAY_IS_PTR(loc)) location = clay_string_data(CLAY_OBJ(loc));
        }

        char http_header[512];
        if (location) {
            snprintf(http_header, sizeof(http_header),
                "HTTP/1.1 %d %s\r\nLocation: %s\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n",
                resp_status, http_status_text(resp_status), location);
        } else {
            snprintf(http_header, sizeof(http_header),
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                resp_status, http_status_text(resp_status), content_type, strlen(resp_body));
        }

        send(client_fd, http_header, strlen(http_header), 0);
        if (!location)
            send(client_fd, resp_body, strlen(resp_body), 0);
        close(client_fd);

        clay_release(CLAY_OBJ(req));
        if (resp != CLAY_NIL && CLAY_IS_PTR(resp)) clay_release(CLAY_OBJ(resp));
    }

    close(server_fd);
    return CLAY_NIL;
}

#endif /* _WIN32 server */

/* Http parseForm(str) — parse URL-encoded form data into a Dict */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

static char *url_decode(const char *src, size_t len) {
    char *out = (char *)malloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '+') {
            out[j++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            int h = hex_digit(src[i+1]), l = hex_digit(src[i+2]);
            if (h >= 0 && l >= 0) { out[j++] = (char)(h * 16 + l); i += 2; }
            else out[j++] = src[i];
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

static ClayValue http_parse_form(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_INT(argv[0]) || argv[0] == CLAY_NIL) {
        return CLAY_VAL(clay_dict_new());
    }
    const char *data = clay_string_data(CLAY_OBJ(argv[0]));
    size_t data_len = strlen(data);
    ClayObject *dict = clay_dict_new();

    const char *p = data;
    while (p < data + data_len) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : (size_t)(data + data_len - p);
        const char *eq = memchr(p, '=', pair_len);
        if (eq) {
            char *key = url_decode(p, (size_t)(eq - p));
            char *val = url_decode(eq + 1, pair_len - (size_t)(eq - p) - 1);
            clay_dict_set(dict, key, CLAY_VAL(clay_string_new(val)));
            free(key);
            free(val);
        }
        p += pair_len + 1;
    }
    return CLAY_VAL(dict);
}

/* Register Http methods */
void clay_http_init(ClayObject *http) {
    clay_set_method(http, "get", http_get);
    clay_set_method(http, "post", http_post);
    clay_set_method(http, "serve", http_serve);
    clay_set_method(http, "response", http_response);
    clay_set_method(http, "jsonResponse", http_json_response);
    clay_set_method(http, "status", resp_status);
    clay_set_method(http, "body", resp_body);
    clay_set_method(http, "method", req_method);
    clay_set_method(http, "path", req_path);
    clay_set_method(http, "query", req_query);
    clay_set_method(http, "params", req_params);
    clay_set_method(http, "parseForm", http_parse_form);
}
