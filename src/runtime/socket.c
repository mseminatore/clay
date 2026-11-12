/*
 * socket.c - Socket prototype: TCP and UDP sockets for the Clay runtime
 *
 *   Socket connect(host, port [, timeoutMs])     TCP client
 *   Socket listen(port [, host [, backlog]])     TCP server (host defaults to 127.0.0.1)
 *   Socket udp([port [, host]])                  UDP socket (bound if a port is given)
 *   Socket resolve(host)                         Array of the host's addresses
 *   Socket select(sockets [, timeoutMs])         the sockets that are ready to read (0: just look)
 *
 *   s read([max])         up to max bytes (at least one); nil when the peer has closed
 *   s readExactly(n)      exactly n bytes; raises if the peer closes first
 *   s write(data)         all of data; returns the byte count
 *   s accept([timeoutMs]) a listening socket's next connection; with a timeout, nil if
 *                         none came in time (0 just looks)
 *   s sendTo(host, port, data) / s receiveFrom([max])    UDP
 *   s setTimeout(ms)      limit for read/write/receiveFrom (0: wait forever); returns s
 *   s noDelay(flag), s shutdownWrite, s close, s isOpen
 *   s localPort, s localAddress, s remotePort, s remoteAddress
 *
 * Data is a String: a sequence of bytes that may hold NULs. Failures raise
 * SocketError; an expired timeout raises TimeoutError. Calls block the whole
 * program (the VM, fibers and all) until they finish or time out.
 *
 * A socket object keeps its descriptor in the integer slot _sock (-1 once it is
 * closed); memory.c closes it if the object is freed while still open.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define sock_errno() WSAGetLastError()
#define E_WOULDBLOCK WSAEWOULDBLOCK
#define E_INPROGRESS WSAEWOULDBLOCK
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#define sock_errno() errno
#define E_WOULDBLOCK EWOULDBLOCK
#define E_INPROGRESS EINPROGRESS
#endif

#include "clay.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKET_MAX_READ (64 * 1024 * 1024)
#define SOCKET_DEFAULT_READ 4096

extern ClayObject *clay_Socket;

/* --- platform helpers --- */

static void net_init(void) {
#ifdef _WIN32
    static int ready = 0;
    if (!ready) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        ready = 1;
    }
#endif
}

static const char *net_error_text(int err) {
#ifdef _WIN32
    static char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             (DWORD)err, 0, buf, (DWORD)sizeof(buf), NULL);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = '\0';
    return n ? buf : "network error";
#else
    return strerror(err);
#endif
}

/* wait until fd is readable (or writable); 1 ready, 0 timed out, -1 error.
 * timeout_ms < 0 waits forever */
static int wait_fd(sock_t fd, int for_write, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p;
    p.fd = fd;
    p.events = for_write ? POLLWRNORM : POLLRDNORM;
    p.revents = 0;
    int r = WSAPoll(&p, 1, timeout_ms);
#else
    struct pollfd p;
    p.fd = fd;
    p.events = for_write ? POLLOUT : POLLIN;
    p.revents = 0;
    int r;
    do { r = poll(&p, 1, timeout_ms); } while (r < 0 && errno == EINTR);
#endif
    return r > 0 ? 1 : (r == 0 ? 0 : -1);
}

static void set_blocking(sock_t fd, int blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

/* no SIGPIPE when writing to a connection the peer has closed */
static void no_sigpipe(sock_t fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
#else
    (void)fd;
#endif
}

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* --- errors --- */

static void fail(const char *type, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    clay_raise(CLAY_VAL(clay_error_new(msg, type)));
}

static void fail_errno(const char *who, int err) {
    fail("SocketError", "%s: %s", who, net_error_text(err));
}

/* --- socket objects --- */

/* a new Socket object around fd (the object owns it) */
static ClayValue wrap(sock_t fd, const char *kind) {
    ClayObject *obj = clay_clone(clay_Socket);
    clay_set_slot(obj, "_sock", clay_int((intptr_t)fd));
    clay_set_slot(obj, "_timeout", clay_int(0));
    clay_set_slot_take(obj, "_kind", CLAY_VAL(clay_string_new(kind)));
    return CLAY_VAL(obj);
}

/* the descriptor of a socket object; raises unless it is a socket (and open) */
static sock_t fd_of(ClayObject *self, const char *who, int must_be_open) {
    ClayValue v = clay_get_slot(self, "_sock");
    if (!CLAY_IS_INT(v)) {
        fail("TypeError", "%s: not a socket (make one with Socket connect, listen or udp)", who);
        return SOCK_BAD;
    }
    intptr_t fd = clay_to_int(v);
    if (fd < 0) {
        if (must_be_open) fail("SocketError", "%s: the socket is closed", who);
        return SOCK_BAD;
    }
    return (sock_t)fd;
}

static int timeout_of(ClayObject *self) {
    ClayValue v = clay_get_slot(self, "_timeout");
    intptr_t ms = CLAY_IS_INT(v) ? clay_to_int(v) : 0;
    return ms > 0 ? (int)ms : -1;
}

/* called by memory.c when an object that owns a descriptor is freed */
void clay_socket_free(intptr_t fd) {
    if (fd >= 0) sock_close((sock_t)fd);
}

static int port_arg(int argc, ClayValue *argv, int i, const char *who, int allow_zero) {
    intptr_t p = clay_arg_integer(argc, argv, i, who);
    if (p < (allow_zero ? 0 : 1) || p > 65535) {
        fail("ArgumentError", "%s: the port must be %s to 65535", who, allow_zero ? "0" : "1");
        return 0;
    }
    return (int)p;
}

static int timeout_arg(int argc, ClayValue *argv, int i, const char *who) {
    if (argc <= i || CLAY_IS_NIL(argv[i])) return -1;
    intptr_t ms = clay_arg_integer(argc, argv, i, who);
    if (ms < 0) {
        fail("ArgumentError", "%s: the timeout must not be negative", who);
        return -1;
    }
    return ms == 0 ? -1 : (int)ms;
}

/* "host" of a sockaddr, as text */
static void addr_text(const struct sockaddr *sa, socklen_t len, char *host, size_t hsz, int *port) {
    char serv[16] = "0";
    host[0] = '\0';
    if (getnameinfo(sa, len, host, (socklen_t)hsz, serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        snprintf(host, hsz, "?");
    *port = atoi(serv);
}

/* --- Socket connect(host, port [, timeoutMs]) --- */

static ClayValue socket_connect(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    net_init();
    const char *host = clay_arg_string(argc, argv, 0, "Socket connect");
    int port = port_arg(argc, argv, 1, "Socket connect", 0);
    int timeout = timeout_arg(argc, argv, 2, "Socket connect");

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, portbuf, &hints, &res);
    if (gai != 0) {
        fail("SocketError", "Socket connect: cannot resolve '%s': %s", host, gai_strerror(gai));
        return CLAY_NIL;
    }

    int last_err = 0, timed_out = 0;
    sock_t fd = SOCK_BAD;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_BAD) { last_err = sock_errno(); continue; }
        no_sigpipe(s);
        int ok;
        if (timeout < 0) {
            ok = connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0;
            if (!ok) last_err = sock_errno();
        } else {
            set_blocking(s, 0);
            int r = connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen);
            ok = r == 0;
            if (!ok) {
                int e = sock_errno();
                if (e == E_INPROGRESS || e == E_WOULDBLOCK) {
                    int w = wait_fd(s, 1, timeout);
                    if (w == 0) { timed_out = 1; }
                    else if (w > 0) {
                        int soerr = 0;
                        socklen_t sl = sizeof(soerr);
                        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
                        if (soerr == 0) ok = 1; else last_err = soerr;
                    } else last_err = sock_errno();
                } else last_err = e;
            }
            if (ok) set_blocking(s, 1);
        }
        if (ok) { fd = s; break; }
        sock_close(s);
    }
    freeaddrinfo(res);
    if (fd == SOCK_BAD) {
        if (timed_out) fail("TimeoutError", "Socket connect: timed out connecting to %s:%d", host, port);
        else fail("SocketError", "Socket connect: %s (%s:%d)", net_error_text(last_err), host, port);
        return CLAY_NIL;
    }
    return wrap(fd, "tcp");
}

/* --- Socket listen(port [, host [, backlog]]) --- */

static ClayValue socket_listen(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    net_init();
    int port = port_arg(argc, argv, 0, "Socket listen", 1);
    const char *host = "127.0.0.1";       /* loopback unless the caller asks for more */
    if (argc > 1 && !CLAY_IS_NIL(argv[1])) host = clay_arg_string(argc, argv, 1, "Socket listen");
    int backlog = 128;
    if (argc > 2 && !CLAY_IS_NIL(argv[2])) {
        intptr_t b = clay_arg_integer(argc, argv, 2, "Socket listen");
        if (b < 1) { fail("ArgumentError", "Socket listen: the backlog must be at least 1"); return CLAY_NIL; }
        backlog = b > 4096 ? 4096 : (int)b;
    }

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int gai = getaddrinfo(host, portbuf, &hints, &res);
    if (gai != 0) {
        fail("SocketError", "Socket listen: cannot resolve '%s': %s", host, gai_strerror(gai));
        return CLAY_NIL;
    }
    int last_err = 0;
    sock_t fd = SOCK_BAD;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_BAD) { last_err = sock_errno(); continue; }
        no_sigpipe(s);
#ifndef _WIN32
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#endif
#ifdef IPV6_V6ONLY
        if (ai->ai_family == AF_INET6) {
            int off = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof(off));
        }
#endif
        if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0 && listen(s, backlog) == 0) { fd = s; break; }
        last_err = sock_errno();
        sock_close(s);
    }
    freeaddrinfo(res);
    if (fd == SOCK_BAD) {
        fail("SocketError", "Socket listen: %s (%s:%d)", net_error_text(last_err), host, port);
        return CLAY_NIL;
    }
    return wrap(fd, "listen");
}

/* --- Socket udp([port [, host]]) --- */

static ClayValue socket_udp(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    net_init();
    int bind_it = argc > 0 && !CLAY_IS_NIL(argv[0]);
    int port = bind_it ? port_arg(argc, argv, 0, "Socket udp", 1) : 0;
    const char *host = "127.0.0.1";
    if (argc > 1 && !CLAY_IS_NIL(argv[1])) host = clay_arg_string(argc, argv, 1, "Socket udp");

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    int gai = getaddrinfo(bind_it ? host : (host[0] ? host : NULL), bind_it ? portbuf : "0", &hints, &res);
    if (gai != 0) {
        fail("SocketError", "Socket udp: cannot resolve '%s': %s", host, gai_strerror(gai));
        return CLAY_NIL;
    }
    int last_err = 0;
    sock_t fd = SOCK_BAD;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_BAD) { last_err = sock_errno(); continue; }
        if (!bind_it || bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) { fd = s; break; }
        last_err = sock_errno();
        sock_close(s);
    }
    freeaddrinfo(res);
    if (fd == SOCK_BAD) {
        fail("SocketError", "Socket udp: %s", net_error_text(last_err));
        return CLAY_NIL;
    }
    return wrap(fd, "udp");
}

/* --- Socket resolve(host) --- */

static ClayValue socket_resolve(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    net_init();
    const char *host = clay_arg_string(argc, argv, 0, "Socket resolve");
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, NULL, &hints, &res);
    if (gai != 0) {
        fail("SocketError", "Socket resolve: cannot resolve '%s': %s", host, gai_strerror(gai));
        return CLAY_NIL;
    }
    ClayObject *arr = clay_array_new();
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        char text[128];
        int port;
        addr_text(ai->ai_addr, (socklen_t)ai->ai_addrlen, text, sizeof(text), &port);
        /* each address once */
        int seen = 0;
        for (size_t i = 0; i < clay_array_size(arr) && !seen; i++)
            seen = strcmp(clay_string_data(CLAY_OBJ(clay_array_at(arr, i))), text) == 0;
        if (seen) continue;
        ClayObject *s = clay_string_new(text);
        clay_array_append(arr, CLAY_VAL(s));
        clay_release(s);
    }
    freeaddrinfo(res);
    return CLAY_VAL(arr);
}

/* --- Socket select(sockets [, timeoutMs]) --- */

static ClayValue socket_select(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    net_init();
    if (argc < 1 || !CLAY_IS_TYPE(argv[0], CLAY_TYPE_ARRAY)) {
        fail("TypeError", "Socket select: expects an Array of sockets");
        return CLAY_NIL;
    }
    int timeout = -1;                                      /* forever, unless given (0 just looks) */
    if (argc > 1 && !CLAY_IS_NIL(argv[1])) {
        intptr_t ms = clay_arg_integer(argc, argv, 1, "Socket select");
        if (ms < 0) { fail("ArgumentError", "Socket select: the timeout must not be negative"); return CLAY_NIL; }
        timeout = (int)ms;
    }
    ClayObject *list = CLAY_OBJ(argv[0]);
    size_t n = clay_array_size(list);
    ClayObject *ready = clay_array_new();
    if (n == 0) {
        /* nothing to wait for: just the timeout */
        if (timeout > 0) {
#ifdef _WIN32
            Sleep((DWORD)timeout);
#else
            struct timespec ts = { timeout / 1000, (timeout % 1000) * 1000000L };
            nanosleep(&ts, NULL);
#endif
        }
        return CLAY_VAL(ready);
    }
#ifdef _WIN32
    WSAPOLLFD *fds = (WSAPOLLFD *)calloc(n, sizeof(WSAPOLLFD));
#else
    struct pollfd *fds = (struct pollfd *)calloc(n, sizeof(struct pollfd));
#endif
    if (!fds) { clay_release(ready); fail("SocketError", "Socket select: out of memory"); return CLAY_NIL; }
    for (size_t i = 0; i < n; i++) {
        ClayValue v = clay_array_at(list, i);
        if (!CLAY_IS_OBJ(v)) { free(fds); clay_release(ready); fail("TypeError", "Socket select: expects an Array of sockets"); return CLAY_NIL; }
        sock_t fd = fd_of(CLAY_OBJ(v), "Socket select", 1);   /* raises if not an open socket */
        fds[i].fd = fd;
#ifdef _WIN32
        fds[i].events = POLLRDNORM;
#else
        fds[i].events = POLLIN;
#endif
    }
    int r;
#ifdef _WIN32
    r = WSAPoll(fds, (ULONG)n, timeout);
#else
    do { r = poll(fds, (nfds_t)n, timeout); } while (r < 0 && errno == EINTR);
#endif
    if (r < 0) { int e = sock_errno(); free(fds); clay_release(ready); fail_errno("Socket select", e); return CLAY_NIL; }
    for (size_t i = 0; i < n; i++)
        if (fds[i].revents) clay_array_append(ready, clay_array_at(list, i));
    free(fds);
    return CLAY_VAL(ready);
}

/* --- reading and writing --- */

/* wait for the socket to be readable within its timeout; raises TimeoutError */
static void await_read(ClayObject *self, sock_t fd, const char *who) {
    int ms = timeout_of(self);
    if (ms < 0) return;
    int w = wait_fd(fd, 0, ms);
    if (w == 0) fail("TimeoutError", "%s: timed out", who);
    else if (w < 0) fail_errno(who, sock_errno());
}

static ClayValue socket_read(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket read", 1);
    intptr_t max = SOCKET_DEFAULT_READ;
    if (argc > 0 && !CLAY_IS_NIL(argv[0])) {
        max = clay_arg_integer(argc, argv, 0, "Socket read");
        if (max < 1) { fail("ArgumentError", "Socket read: max must be at least 1"); return CLAY_NIL; }
        if (max > SOCKET_MAX_READ) max = SOCKET_MAX_READ;
    }
    await_read(self, fd, "Socket read");
    char *buf = (char *)malloc((size_t)max);
    if (!buf) { fail("SocketError", "Socket read: out of memory"); return CLAY_NIL; }
    int n;
    for (;;) {
        n = (int)recv(fd, buf, (int)max, 0);
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        break;
    }
    if (n < 0) { int e = sock_errno(); free(buf); fail_errno("Socket read", e); return CLAY_NIL; }
    ClayValue result = n == 0 ? CLAY_NIL : CLAY_VAL(clay_string_new_len(buf, (size_t)n));
    free(buf);
    return result;
}

static ClayValue socket_readExactly(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket readExactly", 1);
    intptr_t want = clay_arg_integer(argc, argv, 0, "Socket readExactly");
    if (want < 0 || want > SOCKET_MAX_READ) {
        fail("ArgumentError", "Socket readExactly: n must be from 0 to %d", SOCKET_MAX_READ);
        return CLAY_NIL;
    }
    char *buf = (char *)malloc((size_t)want + 1);
    if (!buf) { fail("SocketError", "Socket readExactly: out of memory"); return CLAY_NIL; }
    size_t got = 0;
    while (got < (size_t)want) {
        int ms = timeout_of(self);
        if (ms >= 0) {
            int w = wait_fd(fd, 0, ms);
            if (w == 0) { free(buf); fail("TimeoutError", "Socket readExactly: timed out after %d of %d bytes", (int)got, (int)want); return CLAY_NIL; }
            if (w < 0) { int e = sock_errno(); free(buf); fail_errno("Socket readExactly", e); return CLAY_NIL; }
        }
        int n = (int)recv(fd, buf + got, (int)((size_t)want - got), 0);
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n < 0) { int e = sock_errno(); free(buf); fail_errno("Socket readExactly", e); return CLAY_NIL; }
        if (n == 0) {
            free(buf);
            fail("SocketError", "Socket readExactly: the connection closed after %d of %d bytes", (int)got, (int)want);
            return CLAY_NIL;
        }
        got += (size_t)n;
    }
    ClayValue result = CLAY_VAL(clay_string_new_len(buf, got));
    free(buf);
    return result;
}

static ClayValue socket_write(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket write", 1);
    clay_arg_string(argc, argv, 0, "Socket write");        /* raises unless a String */
    ClayObject *data = CLAY_OBJ(argv[0]);
    const char *p = data->as.string.data;
    size_t left = data->as.string.length;
    size_t total = left;
    while (left > 0) {
        int ms = timeout_of(self);
        if (ms >= 0) {
            int w = wait_fd(fd, 1, ms);
            if (w == 0) { fail("TimeoutError", "Socket write: timed out after %d of %d bytes", (int)(total - left), (int)total); return CLAY_NIL; }
            if (w < 0) { fail_errno("Socket write", sock_errno()); return CLAY_NIL; }
        }
        int chunk = left > (size_t)(1 << 30) ? (1 << 30) : (int)left;
        int n = (int)send(fd, p, chunk, SEND_FLAGS);
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n < 0) { fail_errno("Socket write", sock_errno()); return CLAY_NIL; }
        p += n;
        left -= (size_t)n;
    }
    return clay_int((intptr_t)total);
}

/* --- accept --- */

static ClayValue socket_accept(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket accept", 1);
    if (argc > 0 && !CLAY_IS_NIL(argv[0])) {
        intptr_t ms = clay_arg_integer(argc, argv, 0, "Socket accept");
        if (ms < 0) { fail("ArgumentError", "Socket accept: the timeout must not be negative"); return CLAY_NIL; }
        int w = wait_fd(fd, 0, (int)ms);                   /* 0 just looks */
        if (w == 0) return CLAY_NIL;                       /* nobody came */
        if (w < 0) { fail_errno("Socket accept", sock_errno()); return CLAY_NIL; }
    }
    sock_t c;
    for (;;) {
        c = accept(fd, NULL, NULL);
#ifndef _WIN32
        if (c == SOCK_BAD && errno == EINTR) continue;
#endif
        break;
    }
    if (c == SOCK_BAD) { fail_errno("Socket accept", sock_errno()); return CLAY_NIL; }
    no_sigpipe(c);
    return wrap(c, "tcp");
}

/* --- UDP --- */

static ClayValue socket_sendTo(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket sendTo", 1);
    const char *host = clay_arg_string(argc, argv, 0, "Socket sendTo");
    int port = port_arg(argc, argv, 1, "Socket sendTo", 0);
    clay_arg_string(argc, argv, 2, "Socket sendTo");
    ClayObject *data = CLAY_OBJ(argv[2]);

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    int gai = getaddrinfo(host, portbuf, &hints, &res);
    if (gai != 0) {
        fail("SocketError", "Socket sendTo: cannot resolve '%s': %s", host, gai_strerror(gai));
        return CLAY_NIL;
    }
    int n = (int)sendto(fd, data->as.string.data, (int)data->as.string.length, SEND_FLAGS,
                        res->ai_addr, (socklen_t)res->ai_addrlen);
    int err = n < 0 ? sock_errno() : 0;
    freeaddrinfo(res);
    if (n < 0) { fail_errno("Socket sendTo", err); return CLAY_NIL; }
    return clay_int(n);
}

static ClayValue socket_receiveFrom(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket receiveFrom", 1);
    intptr_t max = 65535;
    if (argc > 0 && !CLAY_IS_NIL(argv[0])) {
        max = clay_arg_integer(argc, argv, 0, "Socket receiveFrom");
        if (max < 1) { fail("ArgumentError", "Socket receiveFrom: max must be at least 1"); return CLAY_NIL; }
        if (max > SOCKET_MAX_READ) max = SOCKET_MAX_READ;
    }
    await_read(self, fd, "Socket receiveFrom");
    char *buf = (char *)malloc((size_t)max);
    if (!buf) { fail("SocketError", "Socket receiveFrom: out of memory"); return CLAY_NIL; }
    struct sockaddr_storage from;
    socklen_t flen = sizeof(from);
    int n;
    for (;;) {
        flen = sizeof(from);
        n = (int)recvfrom(fd, buf, (int)max, 0, (struct sockaddr *)&from, &flen);
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        break;
    }
    if (n < 0) { int e = sock_errno(); free(buf); fail_errno("Socket receiveFrom", e); return CLAY_NIL; }
    char host[128];
    int port;
    addr_text((struct sockaddr *)&from, flen, host, sizeof(host), &port);

    ClayObject *result = clay_array_new();
    ClayObject *payload = clay_string_new_len(buf, (size_t)n);
    ClayObject *hs = clay_string_new(host);
    clay_array_append(result, CLAY_VAL(payload));
    clay_array_append(result, CLAY_VAL(hs));
    clay_array_append(result, clay_int(port));
    clay_release(payload);
    clay_release(hs);
    free(buf);
    return CLAY_VAL(result);
}

/* --- the rest --- */

static ClayValue socket_close(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    sock_t fd = fd_of(self, "Socket close", 0);
    if (fd != SOCK_BAD) {
        sock_close(fd);
        clay_set_slot(self, "_sock", clay_int(-1));
    }
    return CLAY_NIL;
}

static ClayValue socket_isOpen(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return CLAY_BOOL(fd_of(self, "Socket isOpen", 0) != SOCK_BAD);
}

static ClayValue socket_setTimeout(ClayObject *self, int argc, ClayValue *argv) {
    fd_of(self, "Socket setTimeout", 1);
    intptr_t ms = 0;
    if (argc > 0 && !CLAY_IS_NIL(argv[0])) {
        ms = clay_arg_integer(argc, argv, 0, "Socket setTimeout");
        if (ms < 0) { fail("ArgumentError", "Socket setTimeout: the timeout must not be negative"); return CLAY_NIL; }
    }
    clay_set_slot(self, "_timeout", clay_int(ms));
    return CLAY_VAL(self);
}

static ClayValue socket_noDelay(ClayObject *self, int argc, ClayValue *argv) {
    sock_t fd = fd_of(self, "Socket noDelay", 1);
    int on = argc < 1 || (argv[0] != CLAY_FALSE && !CLAY_IS_NIL(argv[0]));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    return CLAY_VAL(self);
}

static ClayValue socket_shutdownWrite(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    sock_t fd = fd_of(self, "Socket shutdownWrite", 1);
#ifdef _WIN32
    shutdown(fd, SD_SEND);
#else
    shutdown(fd, SHUT_WR);
#endif
    return CLAY_VAL(self);
}

static int local_name(ClayObject *self, const char *who, char *host, size_t hsz, int *port, int peer) {
    sock_t fd = fd_of(self, who, 1);
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    int r = peer ? getpeername(fd, (struct sockaddr *)&sa, &len) : getsockname(fd, (struct sockaddr *)&sa, &len);
    if (r != 0) { fail_errno(who, sock_errno()); return 0; }
    addr_text((struct sockaddr *)&sa, len, host, hsz, port);
    return 1;
}

static ClayValue socket_localPort(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char host[128]; int port = 0;
    if (!local_name(self, "Socket localPort", host, sizeof(host), &port, 0)) return CLAY_NIL;
    return clay_int(port);
}

static ClayValue socket_localAddress(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char host[128]; int port = 0;
    if (!local_name(self, "Socket localAddress", host, sizeof(host), &port, 0)) return CLAY_NIL;
    return CLAY_VAL(clay_string_new(host));
}

static ClayValue socket_remotePort(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char host[128]; int port = 0;
    if (!local_name(self, "Socket remotePort", host, sizeof(host), &port, 1)) return CLAY_NIL;
    return clay_int(port);
}

static ClayValue socket_remoteAddress(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char host[128]; int port = 0;
    if (!local_name(self, "Socket remoteAddress", host, sizeof(host), &port, 1)) return CLAY_NIL;
    return CLAY_VAL(clay_string_new(host));
}

static ClayValue socket_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue v = clay_get_slot(self, "_sock");
    if (!CLAY_IS_INT(v)) return CLAY_VAL(clay_string_new("Socket"));
    ClayValue kind = clay_get_slot(self, "_kind");
    char buf[96];
    snprintf(buf, sizeof(buf), "Socket(%s, %s)",
             CLAY_IS_TYPE(kind, CLAY_TYPE_STRING) ? clay_string_data(CLAY_OBJ(kind)) : "?",
             clay_to_int(v) >= 0 ? "open" : "closed");
    return CLAY_VAL(clay_string_new(buf));
}

void clay_socket_init(ClayObject *proto) {
    clay_set_method(proto, "connect", socket_connect);
    clay_set_method(proto, "listen", socket_listen);
    clay_set_method(proto, "udp", socket_udp);
    clay_set_method(proto, "resolve", socket_resolve);
    clay_set_method(proto, "select", socket_select);
    clay_set_method(proto, "read", socket_read);
    clay_set_method(proto, "readExactly", socket_readExactly);
    clay_set_method(proto, "write", socket_write);
    clay_set_method(proto, "accept", socket_accept);
    clay_set_method(proto, "sendTo", socket_sendTo);
    clay_set_method(proto, "receiveFrom", socket_receiveFrom);
    clay_set_method(proto, "close", socket_close);
    clay_set_method(proto, "isOpen", socket_isOpen);
    clay_set_method(proto, "setTimeout", socket_setTimeout);
    clay_set_method(proto, "noDelay", socket_noDelay);
    clay_set_method(proto, "shutdownWrite", socket_shutdownWrite);
    clay_set_method(proto, "localPort", socket_localPort);
    clay_set_method(proto, "localAddress", socket_localAddress);
    clay_set_method(proto, "remotePort", socket_remotePort);
    clay_set_method(proto, "remoteAddress", socket_remoteAddress);
    clay_set_method(proto, "toString", socket_toString);
}
