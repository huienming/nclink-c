/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - portable TCP sockets.
 *
 * Small wrapper over Winsock 2 / BSD sockets. Blocking sockets with explicit
 * timeouts, which is all the NC-Link transports need (MQTT client today, HTTP
 * server next). TLS is not provided; MQTT over "ssl://" reports
 * NCL_ERR_NOT_SUPPORTED.
 */
#ifndef NCL_SOCKET_H
#define NCL_SOCKET_H

#include <stdbool.h>
#include <stddef.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_socket ncl_socket;

/** Initialise the platform networking stack (idempotent). */
ncl_err ncl_socket_system_init(void);
void    ncl_socket_system_shutdown(void);

/**
 * Ask for the networking stack to be released once every socket is closed.
 * Optional: the stack otherwise stays up for the process lifetime, which is
 * what a long running server wants (a socket release must never tear the stack
 * down while other sockets are still in use).
 */
void ncl_socket_system_release(void);

/**
 * Connect to @p host:@p port.
 * @param timeout_ms connection timeout; 0 means "use the default" (10 s).
 * @param err        optional buffer receiving a diagnostic on failure.
 */
ncl_socket *ncl_socket_connect(const char *host, unsigned port,
                               unsigned timeout_ms, char *err, size_t err_len);

/** Create a listening socket bound to @p port (0 picks an ephemeral port). */
ncl_socket *ncl_socket_listen(unsigned port, char *err, size_t err_len);

/** Accept one connection; returns NULL on timeout or error. */
ncl_socket *ncl_socket_accept(ncl_socket *listener, unsigned timeout_ms);

/** Local port of a bound socket, or 0 when unknown. */
unsigned ncl_socket_local_port(const ncl_socket *s);

/**
 * Local address of @p s as text ("192.168.1.7" or "fe80::1%12"), which is the
 * address a peer should use to reach this end. Used by the FTP server to
 * advertise a passive data port and by the FTP client to build PORT/EPRT.
 * Returns NCL_ERR when the socket is closed.
 */
ncl_err ncl_socket_local_ip(const ncl_socket *s, char *buf, size_t buf_len);

/** Peer address of @p s as text. Returns NCL_ERR when the socket is closed. */
ncl_err ncl_socket_peer_ip(const ncl_socket *s, char *buf, size_t buf_len);

/**
 * First non-loopback IPv4 address of an up interface: the address a peer on the
 * LAN should use. Returns NULL when there is none.
 */
const char *ncl_net_local_ipv4(void);

/**
 * Build `{"<interface>":"<ipv4>", ...}` in interface enumeration order.
 * Returns a heap string, or NULL when enumeration fails.
 */
char *ncl_net_ip_map_json(void);

/** Send exactly @p len bytes. Returns NCL_OK or NCL_ERR_IO. */
ncl_err ncl_socket_send(ncl_socket *s, const void *data, size_t len);

/**
 * Receive up to @p len bytes.
 * @return number of bytes read, 0 when the peer closed the connection,
 *         NCL_SOCKET_TIMEOUT (-2) on timeout, -1 on error.
 */
int ncl_socket_recv(ncl_socket *s, void *buf, size_t len, unsigned timeout_ms);

/** Sentinel returned by ncl_socket_recv() when nothing arrived in time. */
#define NCL_SOCKET_TIMEOUT (-2)

/** Receive exactly @p len bytes, looping over partial reads. */
ncl_err ncl_socket_recv_exact(ncl_socket *s, void *buf, size_t len,
                              unsigned timeout_ms);

/** Disable Nagle's algorithm (used by the MQTT client). */
void ncl_socket_set_nodelay(ncl_socket *s, bool enable);

/** Enable TCP keep-alive probes. */
void ncl_socket_set_keepalive(ncl_socket *s, bool enable);

void ncl_socket_close(ncl_socket *s);

/**
 * Close the underlying handle without freeing the ncl_socket. Idempotent and
 * safe to call from a thread that is unblocking a blocking read on the socket:
 * the object stays allocated until ncl_socket_close(). Any subsequent send or
 * receive reports an error.
 */
void ncl_socket_shutdown(ncl_socket *s);

/**
 * Parse "tcp://host:port", "mqtt://host:port" or a bare "host:port".
 * Missing scheme and missing port default to tcp and 1883.
 * @param tls set to true when the scheme is ssl:// or tls://.
 */
ncl_err ncl_socket_parse_url(const char *url, char **host, unsigned *port,
                             bool *tls);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SOCKET_H */
