/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - portable TCP socket layer. */
#include "nclink/ncl_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

#if defined(NCL_OS_WINDOWS)
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
typedef SOCKET ncl_sock_handle;
#  define NCL_INVALID_SOCK INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
typedef int ncl_sock_handle;
#  define NCL_INVALID_SOCK (-1)
#endif

struct ncl_socket {
    ncl_sock_handle handle;
    unsigned        local_port;
    ncl_mutex      *lock;      /**< guards @p handle against shutdown races */
};

/* ------------------------------------------------------------ life cycle -- */

static ncl_mutex *g_socket_mutex = NULL;
static bool       g_socket_ready = false;
static int        g_socket_users = 0;
/** Set when the application asks for an explicit teardown. */
static bool       g_socket_explicit_teardown = false;

ncl_err ncl_socket_system_init(void)
{
    if (g_socket_mutex == NULL) {
        g_socket_mutex = ncl_mutex_create();
        if (g_socket_mutex == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    ncl_mutex_lock(g_socket_mutex);
    if (!g_socket_ready) {
#if defined(NCL_OS_WINDOWS)
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            ncl_mutex_unlock(g_socket_mutex);
            return NCL_ERR_CONNECT;
        }
#endif
        g_socket_ready = true;
    }
    g_socket_users++;
    ncl_mutex_unlock(g_socket_mutex);
    return NCL_OK;
}

void ncl_socket_system_shutdown(void)
{
    if (g_socket_mutex == NULL) {
        return;
    }
    ncl_mutex_lock(g_socket_mutex);
    if (g_socket_users > 0) {
        g_socket_users--;
    }
    /* The networking stack is brought up once per process and normally stays
     * up: sockets come and go continuously (one per peer connection), so tying
     * WSAStartup/WSACleanup to individual sockets would tear the stack down
     * while other sockets are still in use. An application that wants it
     * released calls ncl_socket_system_release() once it is done. */
    if (g_socket_users == 0 && g_socket_ready && g_socket_explicit_teardown) {
#if defined(NCL_OS_WINDOWS)
        WSACleanup();
#endif
        g_socket_ready = false;
    }
    ncl_mutex_unlock(g_socket_mutex);
}

/** Request that the networking stack be released once every socket is closed. */
void ncl_socket_system_release(void)
{
    if (g_socket_mutex == NULL) {
        return;
    }
    ncl_mutex_lock(g_socket_mutex);
    g_socket_explicit_teardown = true;
    if (g_socket_users == 0 && g_socket_ready) {
#if defined(NCL_OS_WINDOWS)
        WSACleanup();
#endif
        g_socket_ready = false;
    }
    ncl_mutex_unlock(g_socket_mutex);
}

static void ncl_socket_set_error(char *err, size_t err_len, const char *what)
{
    if (err == NULL || err_len == 0) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    _snprintf_s(err, err_len, _TRUNCATE, "%s failed (WSA error %d)", what,
                WSAGetLastError());
#else
    snprintf(err, err_len, "%s failed: %s", what, strerror(errno));
#endif
}

static bool ncl_socket_would_block(void)
{
#if defined(NCL_OS_WINDOWS)
    int code = WSAGetLastError();
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EAGAIN;
#endif
}

/** Wait until @p s is readable (or writable) with a millisecond timeout. */
static int ncl_socket_wait(ncl_sock_handle handle, bool for_write,
                           unsigned timeout_ms)
{
    fd_set set;
    struct timeval tv;

    FD_ZERO(&set);
    FD_SET(handle, &set);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)(timeout_ms % 1000u) * 1000L;

#if defined(NCL_OS_WINDOWS)
    {
        int rc = select(0, for_write ? NULL : &set, for_write ? &set : NULL,
                        NULL, timeout_ms == 0 ? NULL : &tv);
        return rc; /* >0 ready, 0 timeout, <0 error */
    }
#else
    {
        int rc = select((int)handle + 1, for_write ? NULL : &set,
                        for_write ? &set : NULL, NULL,
                        timeout_ms == 0 ? NULL : &tv);
        if (rc < 0 && errno == EINTR) {
            return 0;
        }
        return rc;
    }
#endif
}

static ncl_socket *ncl_socket_wrap(ncl_sock_handle handle, unsigned local_port)
{
    ncl_socket *s = (ncl_socket *)calloc(1, sizeof(ncl_socket));
    if (s == NULL) {
#if defined(NCL_OS_WINDOWS)
        closesocket(handle);
#else
        close(handle);
#endif
        return NULL;
    }
    s->lock = ncl_mutex_create();
    if (s->lock == NULL) {
#if defined(NCL_OS_WINDOWS)
        closesocket(handle);
#else
        close(handle);
#endif
        free(s);
        return NULL;
    }
    s->handle = handle;
    s->local_port = local_port;
    return s;
}

/* --------------------------------------------------------- connect/listen - */

/** Build a sockaddr for @p host / @p port, trying getaddrinfo first. */
static ncl_err ncl_socket_resolve(const char *host, unsigned port,
                                  struct addrinfo **out, char *err,
                                  size_t err_len)
{
    struct addrinfo hints;
    char service[16];
    int rc;

    snprintf(service, sizeof(service), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, service, &hints, out);
    if (rc != 0) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "cannot resolve %s:%u", host, port);
        }
        return NCL_ERR_CONNECT;
    }
    return NCL_OK;
}

static ncl_err ncl_socket_connect_addr(struct addrinfo *addr, unsigned timeout_ms,
                                       ncl_sock_handle *out, char *err,
                                       size_t err_len)
{
    ncl_sock_handle handle;
    bool nonblocking = false;

    handle = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (handle == NCL_INVALID_SOCK) {
        ncl_socket_set_error(err, err_len, "socket");
        return NCL_ERR_CONNECT;
    }

#if defined(NCL_OS_WINDOWS)
    {
        u_long mode = 1; /* non blocking connect, then back to blocking */
        ioctlsocket(handle, FIONBIO, &mode);
        nonblocking = true;
    }
#else
    {
        int flags = fcntl(handle, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(handle, F_SETFL, flags | O_NONBLOCK);
            nonblocking = true;
        }
    }
#endif
    (void)nonblocking;

    if (connect(handle, addr->ai_addr, (int)addr->ai_addrlen) != 0) {
        if (!ncl_socket_would_block()) {
            ncl_socket_set_error(err, err_len, "connect");
#if defined(NCL_OS_WINDOWS)
            closesocket(handle);
#else
            close(handle);
#endif
            return NCL_ERR_CONNECT;
        }
        if (ncl_socket_wait(handle, true,
                            timeout_ms == 0 ? 10000u : timeout_ms) <= 0) {
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "connect to broker timed out");
            }
#if defined(NCL_OS_WINDOWS)
            closesocket(handle);
#else
            close(handle);
#endif
            return NCL_ERR_TIMEOUT;
        }
        /* Confirm the connection actually succeeded. */
        {
            int so_error = 0;
#if defined(NCL_OS_WINDOWS)
            int len = (int)sizeof(so_error);
#else
            socklen_t len = (socklen_t)sizeof(so_error);
#endif
            if (getsockopt(handle, SOL_SOCKET, SO_ERROR,
                           (char *)&so_error, &len) != 0 ||
                so_error != 0) {
                if (err != NULL && err_len > 0) {
                    snprintf(err, err_len, "connect failed (errno %d)", so_error);
                }
#if defined(NCL_OS_WINDOWS)
                closesocket(handle);
#else
                close(handle);
#endif
                return NCL_ERR_CONNECT;
            }
        }
    }

    /* Back to blocking mode: the client drives timeouts with select(). */
#if defined(NCL_OS_WINDOWS)
    {
        u_long mode = 0;
        ioctlsocket(handle, FIONBIO, &mode);
    }
#else
    {
        int flags = fcntl(handle, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(handle, F_SETFL, flags & ~O_NONBLOCK);
        }
    }
#endif
    *out = handle;
    return NCL_OK;
}

ncl_socket *ncl_socket_connect(const char *host, unsigned port,
                               unsigned timeout_ms, char *err, size_t err_len)
{
    struct addrinfo *addresses = NULL;
    struct addrinfo *addr;
    ncl_sock_handle handle = NCL_INVALID_SOCK;
    ncl_err rc;

    if (host == NULL || host[0] == '\0' || port == 0) {
        return NULL;
    }
    if (ncl_socket_system_init() != NCL_OK) {
        return NULL;
    }
    if (ncl_socket_resolve(host, port, &addresses, err, err_len) != NCL_OK) {
        return NULL;
    }

    rc = NCL_ERR_CONNECT;
    for (addr = addresses; addr != NULL; addr = addr->ai_next) {
        rc = ncl_socket_connect_addr(addr, timeout_ms, &handle, err, err_len);
        if (rc == NCL_OK) {
            break;
        }
    }
    freeaddrinfo(addresses);
    if (rc != NCL_OK) {
        return NULL;
    }
    return ncl_socket_wrap(handle, 0);
}

ncl_socket *ncl_socket_listen(unsigned port, char *err, size_t err_len)
{
    ncl_sock_handle handle;
    struct sockaddr_in addr;
    int reuse = 1;
    int on = 1;

    if (ncl_socket_system_init() != NCL_OK) {
        return NULL;
    }
    handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == NCL_INVALID_SOCK) {
        ncl_socket_set_error(err, err_len, "socket");
        return NULL;
    }
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
               (int)sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(handle, (struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
        ncl_socket_set_error(err, err_len, "bind");
#if defined(NCL_OS_WINDOWS)
        closesocket(handle);
#else
        close(handle);
#endif
        return NULL;
    }
    if (listen(handle, 16) != 0) {
        ncl_socket_set_error(err, err_len, "listen");
#if defined(NCL_OS_WINDOWS)
        closesocket(handle);
#else
        close(handle);
#endif
        return NULL;
    }
    setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
               (int)sizeof(on));

    {
        struct sockaddr_in bound;
        unsigned bound_port = port;
#if defined(NCL_OS_WINDOWS)
        int len = (int)sizeof(bound);
#else
        socklen_t len = (socklen_t)sizeof(bound);
#endif
        if (getsockname(handle, (struct sockaddr *)&bound, &len) == 0) {
            bound_port = ntohs(bound.sin_port);
        }
        return ncl_socket_wrap(handle, bound_port);
    }
}

ncl_socket *ncl_socket_accept(ncl_socket *listener, unsigned timeout_ms)
{
    ncl_sock_handle handle;

    if (listener == NULL || listener->handle == NCL_INVALID_SOCK) {
        return NULL;
    }
    if (ncl_socket_wait(listener->handle, false, timeout_ms) <= 0) {
        return NULL;
    }
    handle = accept(listener->handle, NULL, NULL);
    if (handle == NCL_INVALID_SOCK) {
        return NULL;
    }
    {
        int on = 1;
        setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
                   (int)sizeof(on));
    }
    return ncl_socket_wrap(handle, 0);
}

unsigned ncl_socket_local_port(const ncl_socket *s)
{
    return s != NULL ? s->local_port : 0;
}

/* --------------------------------------------------------- address helpers - */

static ncl_err ncl_socket_name_of(const ncl_socket *s, bool peer, char *buf,
                                  size_t buf_len)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = (socklen_t)sizeof(addr);
    ncl_sock_handle handle;
    int rc;

    if (s == NULL || buf == NULL || buf_len == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    ncl_mutex_lock(s->lock);
    handle = s->handle;
    ncl_mutex_unlock(s->lock);
    if (handle == NCL_INVALID_SOCK) {
        return NCL_ERR_CLOSED;
    }
    memset(&addr, 0, sizeof(addr));
    rc = peer ? getpeername(handle, (struct sockaddr *)&addr, &addr_len)
              : getsockname(handle, (struct sockaddr *)&addr, &addr_len);
    if (rc != 0) {
        return NCL_ERR_IO;
    }
    rc = getnameinfo((struct sockaddr *)&addr, addr_len, buf, (socklen_t)buf_len,
                     NULL, 0, NI_NUMERICHOST);
    if (rc != 0) {
        buf[0] = '\0';
        return NCL_ERR_IO;
    }
    return NCL_OK;
}

ncl_err ncl_socket_local_ip(const ncl_socket *s, char *buf, size_t buf_len)
{
    return ncl_socket_name_of(s, false, buf, buf_len);
}

ncl_err ncl_socket_peer_ip(const ncl_socket *s, char *buf, size_t buf_len)
{
    return ncl_socket_name_of(s, true, buf, buf_len);
}

/* ------------------------------------------------------- interface listing - */

static ncl_strbuf *g_ip_map;   /* scratch used by ncl_net_local_ipv4() */
static char        g_local_ipv4[64];
static ncl_mutex  *g_net_mutex;

static void ncl_net_escape_json(ncl_strbuf *sb, const char *text)
{
    ncl_strbuf_putc(sb, '"');
    for (; text != NULL && *text != '\0'; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            ncl_strbuf_putc(sb, '\\');
            ncl_strbuf_putc(sb, (char)c);
        } else if (c < 0x20) {
            ncl_strbuf_printf(sb, "\\u%04x", c);
        } else {
            ncl_strbuf_putc(sb, (char)c);
        }
    }
    ncl_strbuf_putc(sb, '"');
}

/**
 * Enumerate the IPv4 addresses of up interfaces and render them as JSON.
 * Interface names follow the platform convention on Windows
 * ("eth<index>", "wlan<index>") so log output matches.
 */
static ncl_strbuf *ncl_net_collect(void)
{
    ncl_strbuf *sb = (ncl_strbuf *)calloc(1, sizeof(ncl_strbuf));
    char first[64];

    if (sb == NULL) {
        return NULL;
    }
    ncl_strbuf_init(sb);
    first[0] = '\0';
    ncl_strbuf_puts(sb, "{");
#if defined(NCL_OS_WINDOWS)
    {
        ULONG size = 16 * 1024;
        IP_ADAPTER_ADDRESSES *addrs = NULL;
        IP_ADAPTER_ADDRESSES *cur;
        ULONG rc;
        int iter;
        int written = 0;

        for (iter = 0; iter < 3; iter++) {
            addrs = (IP_ADAPTER_ADDRESSES *)malloc(size);
            if (addrs == NULL) {
                break;
            }
            rc = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST |
                                          GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL, addrs, &size);
            if (rc == ERROR_BUFFER_OVERFLOW) {
                free(addrs);
                addrs = NULL;
                continue;
            }
            break;
        }
        for (cur = addrs; cur != NULL; cur = cur->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *ua;
            const char *ip = NULL;
            char addr_text[64];
            char name[64];

            addr_text[0] = '\0';
            if (cur->OperStatus != IfOperStatusUp ||
                cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                cur->IfType == IF_TYPE_TUNNEL) {
                continue;
            }
            for (ua = cur->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
                struct sockaddr_in *sin;
                if (ua->Address.lpSockaddr == NULL ||
                    ua->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }
                sin = (struct sockaddr_in *)ua->Address.lpSockaddr;
                /* The last IPv4 address of each interface wins. */
                if (inet_ntop(AF_INET, &sin->sin_addr, addr_text,
                              sizeof(addr_text)) != NULL) {
                    ip = addr_text;
                }
            }
            if (ip == NULL) {
                continue;
            }
            snprintf(name, sizeof(name), "%s%lu",
                     cur->IfType == IF_TYPE_IEEE80211 ? "wlan" : "eth",
                     (unsigned long)cur->IfIndex);
            if (written > 0) {
                ncl_strbuf_putc(sb, ',');
            }
            ncl_net_escape_json(sb, name);
            ncl_strbuf_putc(sb, ':');
            ncl_net_escape_json(sb, ip);
            written++;
            if (first[0] == '\0') {
                snprintf(first, sizeof(first), "%s", ip);
            }
        }
        free(addrs);
    }
#else
    {
        char host[256];
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        struct addrinfo *cur;
        int written = 0;
        char ip[64];

        if (gethostname(host, sizeof(host)) == 0) {
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, NULL, &hints, &res) == 0) {
                for (cur = res; cur != NULL; cur = cur->ai_next) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)cur->ai_addr;
                    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                        continue;
                    }
                    if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == NULL) {
                        continue;
                    }
                    if (written > 0) {
                        ncl_strbuf_putc(sb, ',');
                    }
                    ncl_net_escape_json(sb, "eth0");
                    ncl_strbuf_putc(sb, ':');
                    ncl_net_escape_json(sb, ip);
                    written++;
                    if (first[0] == '\0') {
                        snprintf(first, sizeof(first), "%s", ip);
                    }
                }
                freeaddrinfo(res);
            }
        }
    }
#endif
    ncl_strbuf_puts(sb, "}");
    snprintf(g_local_ipv4, sizeof(g_local_ipv4), "%s", first);
    return sb;
}

static void ncl_net_refresh(void)
{
    if (g_net_mutex == NULL) {
        g_net_mutex = ncl_mutex_create();
    }
    if (g_net_mutex == NULL) {
        return;
    }
    ncl_mutex_lock(g_net_mutex);
    if (g_ip_map == NULL) {
        g_ip_map = ncl_net_collect();
    }
    ncl_mutex_unlock(g_net_mutex);
}

const char *ncl_net_local_ipv4(void)
{
    ncl_net_refresh();
    return g_local_ipv4[0] != '\0' ? g_local_ipv4 : NULL;
}

char *ncl_net_ip_map_json(void)
{
    char *out;

    ncl_net_refresh();
    if (g_net_mutex == NULL) {
        return NULL;
    }
    ncl_mutex_lock(g_net_mutex);
    out = g_ip_map != NULL ? ncl_strdup(ncl_strbuf_cstr(g_ip_map)) : NULL;
    ncl_mutex_unlock(g_net_mutex);
    return out;
}

/* ------------------------------------------------------------ data paths -- */

ncl_err ncl_socket_send(ncl_socket *s, const void *data, size_t len)
{
    const unsigned char *cursor = (const unsigned char *)data;
    size_t remaining = len;
    ncl_sock_handle handle;

    if (s == NULL || (data == NULL && len > 0)) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(s->lock);
    handle = s->handle;
    ncl_mutex_unlock(s->lock);
    if (handle == NCL_INVALID_SOCK) {
        return NCL_ERR_CLOSED;
    }
    while (remaining > 0) {
        int sent = send(handle, (const char *)cursor, (int)remaining, 0);
        if (sent <= 0) {
            if (sent < 0 && ncl_socket_would_block()) {
                if (ncl_socket_wait(handle, true, 10000) <= 0) {
                    return NCL_ERR_TIMEOUT;
                }
                continue;
            }
            return NCL_ERR_IO;
        }
        cursor += sent;
        remaining -= (size_t)sent;
    }
    return NCL_OK;
}

int ncl_socket_recv(ncl_socket *s, void *buf, size_t len, unsigned timeout_ms)
{
    int rc;
    ncl_sock_handle handle;

    if (s == NULL || buf == NULL || len == 0) {
        return -1;
    }
    ncl_mutex_lock(s->lock);
    handle = s->handle;
    ncl_mutex_unlock(s->lock);
    if (handle == NCL_INVALID_SOCK) {
        return -1;
    }
    rc = ncl_socket_wait(handle, false, timeout_ms);
    if (rc == 0) {
        return NCL_SOCKET_TIMEOUT;
    }
    if (rc < 0) {
        return -1;
    }
    rc = recv(handle, (char *)buf, (int)len, 0);
    if (rc < 0 && ncl_socket_would_block()) {
        return NCL_SOCKET_TIMEOUT;
    }
    return rc;
}

ncl_err ncl_socket_recv_exact(ncl_socket *s, void *buf, size_t len,
                              unsigned timeout_ms)
{
    unsigned char *cursor = (unsigned char *)buf;
    size_t received = 0;
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;

    while (received < len) {
        int64_t now = ncl_time_monotonic_millis();
        unsigned remaining;
        int rc;

        if (now >= deadline) {
            return NCL_ERR_TIMEOUT;
        }
        remaining = (unsigned)(deadline - now);
        rc = ncl_socket_recv(s, cursor + received, len - received, remaining);
        if (rc == NCL_SOCKET_TIMEOUT) {
            continue;
        }
        if (rc <= 0) {
            return NCL_ERR_IO;
        }
        received += (size_t)rc;
    }
    return NCL_OK;
}

void ncl_socket_set_nodelay(ncl_socket *s, bool enable)
{
    int on = enable ? 1 : 0;
    if (s != NULL) {
        setsockopt(s->handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
                   (int)sizeof(on));
    }
}

void ncl_socket_set_keepalive(ncl_socket *s, bool enable)
{
    int on = enable ? 1 : 0;
    if (s != NULL) {
        setsockopt(s->handle, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on,
                   (int)sizeof(on));
    }
}

void ncl_socket_close(ncl_socket *s)
{
    if (s == NULL) {
        return;
    }
    ncl_socket_shutdown(s);
    ncl_mutex_destroy(s->lock);
    free(s);
}

void ncl_socket_shutdown(ncl_socket *s)
{
    if (s == NULL) {
        return;
    }
    ncl_mutex_lock(s->lock);
    if (s->handle != NCL_INVALID_SOCK) {
#if defined(NCL_OS_WINDOWS)
        shutdown(s->handle, SD_BOTH);
        closesocket(s->handle);
#else
        shutdown(s->handle, SHUT_RDWR);
        close(s->handle);
#endif
        s->handle = NCL_INVALID_SOCK;
    }
    ncl_mutex_unlock(s->lock);
}

/* -------------------------------------------------------------- URL parse - */

ncl_err ncl_socket_parse_url(const char *url, char **host, unsigned *port,
                             bool *tls)
{
    const char *rest;
    const char *colon;
    const char *end;

    if (url == NULL || host == NULL || port == NULL || tls == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *host = NULL;
    *port = 1883;
    *tls = false;

    rest = url;
    if (strstr(url, "://") != NULL) {
        const char *scheme_end = strstr(url, "://");
        size_t scheme_len = (size_t)(scheme_end - url);
        if (scheme_len == 3 && (strncmp(url, "ssl", 3) == 0 ||
                                strncmp(url, "tls", 3) == 0)) {
            *tls = true;
            *port = 8883;
        } else if (scheme_len == 5 && strncmp(url, "mqtts", 5) == 0) {
            *tls = true;
            *port = 8883;
        } else if (scheme_len == 3 && strncmp(url, "tcp", 3) == 0) {
            /* plain TCP */
        } else if (scheme_len == 4 && strncmp(url, "mqtt", 4) == 0) {
            /* plain TCP */
        } else if ((scheme_len == 2 && strncmp(url, "ws", 2) == 0) ||
                   (scheme_len == 3 && strncmp(url, "wss", 3) == 0)) {
            return NCL_ERR_NOT_SUPPORTED; /* websockets are out of scope */
        } else {
            return NCL_ERR_NOT_SUPPORTED;
        }
        rest = scheme_end + 3;
    }

    /* Strip an optional userinfo@ prefix. */
    {
        const char *at = strchr(rest, '@');
        const char *slash = strchr(rest, '/');
        if (at != NULL && (slash == NULL || at < slash)) {
            rest = at + 1;
        }
    }
    /* Stop at the first path separator. */
    end = strchr(rest, '/');
    if (end == NULL) {
        end = rest + strlen(rest);
    }

    colon = NULL;
    {
        const char *p;
        for (p = rest; p < end; p++) {
            if (*p == ':') {
                colon = p;
            }
        }
    }
    if (colon != NULL) {
        long value = strtol(colon + 1, NULL, 10);
        if (value <= 0 || value > 65535) {
            return NCL_ERR_INVALID_ARG;
        }
        *port = (unsigned)value;
        *host = ncl_strndup(rest, (size_t)(colon - rest));
    } else {
        *host = ncl_strndup(rest, (size_t)(end - rest));
    }
    if (*host == NULL || (*host)[0] == '\0') {
        free(*host);
        *host = NULL;
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}
