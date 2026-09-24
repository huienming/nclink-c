/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - FTP client.
 *
 * The command set is the one the NC-Link file channel needs: connect/login,
 * binary transfers, an active data connection by default, and a NOOP based
 * liveness check.
 */
#include "ftp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_platform.h"

struct ncl_ftp_client {
    char          *host;
    unsigned       port;
    char          *user;
    char          *password;
    unsigned       connect_timeout_ms;
    unsigned       io_timeout_ms;
    bool           passive;

    ncl_socket    *ctrl;
    ncl_ftp_reader reader;

    int            reply_code;
    char          *reply_text;

    /* Data connection state, valid between a transfer command and its end. */
    ncl_socket    *pasv_sock;   /**< passive listener, owned by the client */
    char           port_host[64];
    unsigned       port_port;
    bool           have_port;

    /* Streamed transfers: what has been moved, and the test seam that makes a
     * transfer die half way so the resume path can be exercised. */
    long long      bytes_sent;
    long long      bytes_received;
    long long      abort_after; /**< 0 = no abort */
};

/** One piece of a streamed transfer: 256 KiB, the protocol's chunk size. */
#define NCL_FTP_STREAM_CHUNK (1024u * 256u)
/** How often a streamed transfer is retried (each retry resumes). */
#define NCL_FTP_TRANSFER_ATTEMPTS 3

/** Test seam: abort the current transfer after this many bytes (0 = off). */
#define NCL_FTP_ABORT_ENV "NCL_TEST_FTP_ABORT_AFTER"

/* ------------------------------------------------------------------ utils -- */

static void ncl_ftp_client_set_reply(ncl_ftp_client *c, int code,
                                     const char *text)
{
    c->reply_code = code;
    ncl_mem_free(c->reply_text);
    c->reply_text = ncl_strdup(text != NULL ? text : "");
}

static ncl_err ncl_ftp_client_expect(ncl_ftp_client *c, int wanted_prefix,
                                     ncl_strbuf *out)
{
    ncl_strbuf text;
    ncl_err rc;
    int code = 0;

    ncl_strbuf_init(&text);
    rc = ncl_ftp_reader_reply(&c->reader, &code, &text, c->io_timeout_ms);
    if (rc == NCL_OK) {
        ncl_ftp_client_set_reply(c, code, ncl_strbuf_cstr(&text));
        if (out != NULL) {
            ncl_strbuf_puts(out, ncl_strbuf_cstr(&text));
        }
        if (code / 100 != wanted_prefix) {
            rc = NCL_ERR_IO;
        }
    }
    ncl_strbuf_free(&text);
    return rc;
}

static ncl_err ncl_ftp_client_command(ncl_ftp_client *c, const char *fmt, ...)
{
    va_list ap;
    char *line = NULL;
    ncl_err rc;

    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    va_start(ap, fmt);
    rc = ncl_vasprintf(&line, fmt, ap);
    va_end(ap);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_ftp_send_line(c->ctrl, line);
    ncl_mem_free(line);
    return rc;
}

static void ncl_ftp_client_close_data(ncl_ftp_client *c)
{
    if (c->pasv_sock != NULL) {
        ncl_socket_close(c->pasv_sock);
        c->pasv_sock = NULL;
    }
    c->have_port = false;
    c->port_port = 0;
    c->port_host[0] = '\0';
}

/* ------------------------------------------------------- data connections -- */

/**
 * Open the data connection for the next transfer command. Active mode emits
 * PORT (ERR/EPRT is not needed: the server subset is IPv4 only), passive mode
 * emits PASV and connects to the advertised port.
 */
static ncl_socket *ncl_ftp_client_open_data(ncl_ftp_client *c, char *err,
                                            size_t err_len)
{
    ncl_socket *data = NULL;

    ncl_ftp_client_close_data(c);

    if (c->passive) {
        ncl_strbuf text;
        char host[64];
        unsigned port = 0;
        ncl_err rc;

        ncl_strbuf_init(&text);
        rc = ncl_ftp_client_command(c, "PASV");
        if (rc == NCL_OK) {
            rc = ncl_ftp_client_expect(c, 2, &text);
        }
        if (rc == NCL_OK &&
            ncl_ftp_parse_pasv_reply(ncl_strbuf_cstr(&text), host,
                                     sizeof(host), &port) != NCL_OK) {
            rc = NCL_ERR_PARSE;
        }
        ncl_strbuf_free(&text);
        if (rc != NCL_OK) {
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "PASV failed: %s", c->reply_text);
            }
            return NULL;
        }
        /* A server behind NAT may advertise 0.0.0.0; fall back to the control
         * connection's peer address. */
        if (strcmp(host, "0.0.0.0") == 0 && c->ctrl != NULL) {
            if (ncl_socket_peer_ip(c->ctrl, host, sizeof(host)) != NCL_OK) {
                snprintf(host, sizeof(host), "%s", c->host);
            }
        }
        data = ncl_socket_connect(host, port, c->connect_timeout_ms, err,
                                  err_len);
        if (data == NULL) {
            return NULL;
        }
    } else {
        char local_ip[64];
        char arg[64];
        ncl_socket *listener = NULL;
        unsigned port = 0;

        if (ncl_socket_local_ip(c->ctrl, local_ip, sizeof(local_ip)) != NCL_OK) {
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "cannot determine the local address");
            }
            return NULL;
        }
        listener = ncl_socket_listen(0, err, err_len);
        if (listener == NULL) {
            return NULL;
        }
        port = ncl_socket_local_port(listener);
        if (ncl_ftp_format_port_arg(arg, sizeof(arg), local_ip, port) !=
            NCL_OK) {
            ncl_socket_close(listener);
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "cannot build a PORT argument from %s",
                         local_ip);
            }
            return NULL;
        }
        if (ncl_ftp_client_command(c, "PORT %s", arg) != NCL_OK ||
            ncl_ftp_client_expect(c, 2, NULL) != NCL_OK) {
            ncl_socket_close(listener);
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "PORT rejected: %s", c->reply_text);
            }
            return NULL;
        }
        c->pasv_sock = listener;
        snprintf(c->port_host, sizeof(c->port_host), "%s", local_ip);
        c->port_port = port;
        c->have_port = true;
        /* The server dials back once the transfer command arrives. */
        data = listener;
    }
    return data;
}

/** Wait for the server to connect back (active mode) and return the socket. */
static ncl_socket *ncl_ftp_client_accept_data(ncl_ftp_client *c, char *err,
                                              size_t err_len)
{
    ncl_socket *data;

    if (c->pasv_sock == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "no data connection was requested");
        }
        return NULL;
    }
    data = ncl_socket_accept(c->pasv_sock, c->io_timeout_ms);
    if (data == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "the server did not open the data connection");
        }
    }
    return data;
}

/**
 * Send a transfer command (@p verb plus optional @p arg) and hand back the
 * data connection.
 *
 * In passive mode the connection returned by ncl_ftp_client_open_data() is the
 * data connection itself; in active mode the listener is parked in
 * c->pasv_sock and the accepted connection is returned. Callers close the
 * returned socket and then call ncl_ftp_client_close_data() to drop the
 * listener (a no-op in passive mode).
 */
static ncl_err ncl_ftp_client_data_command(ncl_ftp_client *c, const char *verb,
                                           const char *arg,
                                           ncl_socket **out_conn)
{
    char err[256];
    ncl_socket *setup;
    ncl_err rc;

    if (out_conn == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out_conn = NULL;
    err[0] = '\0';
    setup = ncl_ftp_client_open_data(c, err, sizeof(err));
    if (setup == NULL) {
        ncl_log_error("ftp %s: %s", verb, err);
        return NCL_ERR_CONNECT;
    }
    rc = ncl_ftp_client_command(c, "%s%s%s", verb, arg != NULL ? " " : "",
                                arg != NULL ? arg : "");
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 1, NULL);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    if (c->passive) {
        *out_conn = setup;
    } else {
        *out_conn = ncl_ftp_client_accept_data(c, err, sizeof(err));
        if (*out_conn == NULL) {
            ncl_log_error("ftp %s: %s", verb, err);
            return NCL_ERR_CONNECT;
        }
    }
    return NCL_OK;
}

/** Drain @p conn into @p out until the peer closes the data connection. */
static ncl_err ncl_ftp_client_drain(ncl_ftp_client *c, ncl_socket *conn,
                                    ncl_strbuf *out)
{
    char chunk[8192];

    for (;;) {
        int got = ncl_socket_recv(conn, chunk, sizeof(chunk), c->io_timeout_ms);
        if (got == NCL_SOCKET_TIMEOUT) {
            continue;
        }
        if (got < 0) {
            return NCL_ERR_IO;
        }
        if (got == 0) {
            return NCL_OK;
        }
        if (ncl_strbuf_append(out, chunk, (size_t)got) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }
}

/* ------------------------------------------------------------------ login -- */

void ncl_ftp_client_disconnect(ncl_ftp_client *c)
{
    if (c == NULL) {
        return;
    }
    ncl_ftp_client_close_data(c);
    if (c->ctrl != NULL) {
        if (ncl_ftp_client_command(c, "QUIT") == NCL_OK) {
            ncl_ftp_client_expect(c, 2, NULL);
        }
        ncl_socket_close(c->ctrl);
        c->ctrl = NULL;
        ncl_ftp_reader_init(&c->reader, NULL);
    }
}

bool ncl_ftp_client_is_connected(const ncl_ftp_client *c)
{
    return c != NULL && c->ctrl != NULL;
}

static bool ncl_ftp_client_login(ncl_ftp_client *c)
{
    ncl_err rc;

    c->ctrl = ncl_socket_connect(c->host, c->port, c->connect_timeout_ms, NULL,
                                 0);
    if (c->ctrl == NULL) {
        return false;
    }
    ncl_socket_set_nodelay(c->ctrl, true);
    ncl_socket_set_keepalive(c->ctrl, true);
    ncl_ftp_reader_init(&c->reader, c->ctrl);

    rc = ncl_ftp_client_expect(c, 2, NULL); /* greeting */
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_command(c, "USER %s", c->user);
    }
    if (rc == NCL_OK) {
        /* 331 is the normal answer; a permissive server may already be happy. */
        ncl_strbuf text;
        ncl_err read_rc;
        int code = 0;

        ncl_strbuf_init(&text);
        read_rc = ncl_ftp_reader_reply(&c->reader, &code, &text, c->io_timeout_ms);
        if (read_rc == NCL_OK) {
            ncl_ftp_client_set_reply(c, code, ncl_strbuf_cstr(&text));
        }
        ncl_strbuf_free(&text);
        rc = read_rc;
        if (rc == NCL_OK && code == 230) {
            /* already authenticated */
        } else if (rc == NCL_OK && code == 331) {
            rc = ncl_ftp_client_command(c, "PASS %s", c->password);
            if (rc == NCL_OK) {
                rc = ncl_ftp_client_expect(c, 2, NULL);
            }
        } else {
            rc = NCL_ERR_IO;
        }
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_command(c, "TYPE I");
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    if (rc != NCL_OK) {
        ncl_ftp_client_disconnect(c);
        return false;
    }
    return true;
}

bool ncl_ftp_client_noop(ncl_ftp_client *c)
{
    if (c == NULL || c->ctrl == NULL) {
        return false;
    }
    if (ncl_ftp_client_command(c, "NOOP") != NCL_OK) {
        return false;
    }
    return ncl_ftp_client_expect(c, 2, NULL) == NCL_OK;
}

bool ncl_ftp_client_detect(ncl_ftp_client *c)
{
    if (c == NULL) {
        return false;
    }
    if (c->ctrl != NULL && ncl_ftp_client_noop(c)) {
        return true;
    }
    ncl_ftp_client_disconnect(c);
    return ncl_ftp_client_login(c);
}

/* ------------------------------------------------------------------ setup -- */

ncl_ftp_client *ncl_ftp_client_create(const char *host, unsigned port,
                                      const char *user, const char *password)
{
    return ncl_ftp_client_create_ex(host, port, user, password, NULL);
}

ncl_ftp_client *ncl_ftp_client_create_ex(const char *host, unsigned port,
                                         const char *user, const char *password,
                                         const ncl_ftp_client_options *options)
{
    ncl_ftp_client *c;

    if (host == NULL || host[0] == '\0') {
        return NULL;
    }
    c = (ncl_ftp_client *)ncl_mem_calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->host = ncl_strdup(host);
    c->port = port != 0 ? port : (unsigned)NCL_FTP_CLIENT_HOLDER_PORT;
    c->user = ncl_strdup(user != NULL ? user : NCL_FTP_DEFAULT_USER);
    c->password =
        ncl_strdup(password != NULL ? password : NCL_FTP_DEFAULT_PASSWORD);
    c->connect_timeout_ms = NCL_FTP_CONNECT_TIMEOUT_MS;
    c->io_timeout_ms = NCL_FTP_DATA_TIMEOUT_MS;
    c->passive = false; /* active mode by default */
    if (options != NULL) {
        if (options->connect_timeout_ms != 0) {
            c->connect_timeout_ms = options->connect_timeout_ms;
        }
        if (options->io_timeout_ms != 0) {
            c->io_timeout_ms = options->io_timeout_ms;
        }
        c->passive = options->passive;
    }
    ncl_ftp_reader_init(&c->reader, NULL);
    if (c->host == NULL || c->user == NULL || c->password == NULL) {
        ncl_ftp_client_free(c);
        return NULL;
    }
    return c;
}

void ncl_ftp_client_free(ncl_ftp_client *c)
{
    if (c == NULL) {
        return;
    }
    ncl_ftp_client_disconnect(c);
    ncl_mem_free(c->host);
    ncl_mem_free(c->user);
    ncl_mem_free(c->password);
    ncl_mem_free(c->reply_text);
    ncl_mem_free(c);
}

const char *ncl_ftp_client_host(const ncl_ftp_client *c)
{
    return c != NULL ? c->host : NULL;
}

unsigned ncl_ftp_client_port(const ncl_ftp_client *c)
{
    return c != NULL ? c->port : 0;
}

int ncl_ftp_client_reply_code(const ncl_ftp_client *c)
{
    return c != NULL ? c->reply_code : 0;
}

const char *ncl_ftp_client_reply_text(const ncl_ftp_client *c)
{
    return (c != NULL && c->reply_text != NULL) ? c->reply_text : "";
}

void ncl_ftp_client_set_passive(ncl_ftp_client *c, bool passive)
{
    if (c != NULL) {
        c->passive = passive;
    }
}

bool ncl_ftp_client_is_passive(const ncl_ftp_client *c)
{
    return c != NULL && c->passive;
}

/* ------------------------------------------------------------- directories - */

ncl_err ncl_ftp_client_pwd(ncl_ftp_client *c, char *buf, size_t buf_len)
{
    ncl_strbuf text;
    ncl_err rc;
    const char *open;
    const char *close;

    if (c == NULL || buf == NULL || buf_len == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    ncl_strbuf_init(&text);
    rc = ncl_ftp_client_command(c, "PWD");
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, &text);
    }
    if (rc == NCL_OK) {
        const char *body = ncl_strbuf_cstr(&text);
        open = strchr(body, '"');
        close = open != NULL ? strchr(open + 1, '"') : NULL;
        if (open == NULL || close == NULL) {
            rc = NCL_ERR_PARSE;
        } else {
            size_t len = (size_t)(close - open - 1);
            if (len >= buf_len) {
                rc = NCL_ERR_RANGE;
            } else {
                memcpy(buf, open + 1, len);
                buf[len] = '\0';
            }
        }
    }
    ncl_strbuf_free(&text);
    return rc;
}

ncl_err ncl_ftp_client_chdir(ncl_ftp_client *c, const char *path)
{
    ncl_err rc;

    if (c == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_ftp_client_command(c, "CWD %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    return rc;
}

ncl_err ncl_ftp_client_mkdir(ncl_ftp_client *c, const char *path)
{
    ncl_err rc;

    if (c == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_ftp_client_command(c, "MKD %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    return rc;
}

ncl_err ncl_ftp_client_rmdir(ncl_ftp_client *c, const char *path)
{
    ncl_err rc;

    if (c == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_ftp_client_command(c, "RMD %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    return rc;
}

ncl_err ncl_ftp_client_delete(ncl_ftp_client *c, const char *path)
{
    ncl_err rc;

    if (c == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_ftp_client_command(c, "DELE %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    return rc;
}

ncl_err ncl_ftp_client_rename(ncl_ftp_client *c, const char *from,
                              const char *to)
{
    ncl_err rc;

    if (c == NULL || from == NULL || to == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_ftp_client_command(c, "RNFR %s", from);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 3, NULL);
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_command(c, "RNTO %s", to);
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    return rc;
}

ncl_err ncl_ftp_client_size(ncl_ftp_client *c, const char *path,
                            long long *out_size)
{
    ncl_strbuf text;
    ncl_err rc;

    if (c == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_strbuf_init(&text);
    rc = ncl_ftp_client_command(c, "SIZE %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, &text);
    }
    if (rc == NCL_OK && out_size != NULL) {
        const char *body = ncl_strbuf_cstr(&text);
        char *end = NULL;
        long long value = strtoll(body, &end, 10);
        if (end == body) {
            rc = NCL_ERR_PARSE;
        } else {
            *out_size = value;
        }
    }
    ncl_strbuf_free(&text);
    return rc;
}

ncl_err ncl_ftp_client_mdtm(ncl_ftp_client *c, const char *path,
                            int64_t *out_time)
{
    ncl_strbuf text;
    ncl_err rc;

    if (c == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_strbuf_init(&text);
    rc = ncl_ftp_client_command(c, "MDTM %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, &text);
    }
    if (rc == NCL_OK && out_time != NULL) {
        int64_t value = ncl_ftp_parse_mdtm(ncl_strbuf_cstr(&text));
        if (value == 0) {
            rc = NCL_ERR_PARSE;
        } else {
            *out_time = value;
        }
    }
    ncl_strbuf_free(&text);
    return rc;
}

bool ncl_ftp_client_is_dir(ncl_ftp_client *c, const char *path)
{
    ncl_strbuf text;
    char saved[NCL_FTP_PATH_MAX];
    ncl_err rc;
    int code = 0;

    if (c == NULL || path == NULL || c->ctrl == NULL) {
        return false;
    }
    saved[0] = '\0';
    if (ncl_ftp_client_pwd(c, saved, sizeof(saved)) != NCL_OK) {
        saved[0] = '\0';
    }
    ncl_strbuf_init(&text);
    rc = ncl_ftp_client_command(c, "CWD %s", path);
    if (rc == NCL_OK) {
        rc = ncl_ftp_reader_reply(&c->reader, &code, &text, c->io_timeout_ms);
        if (rc == NCL_OK) {
            ncl_ftp_client_set_reply(c, code, ncl_strbuf_cstr(&text));
        }
    }
    ncl_strbuf_free(&text);
    if (rc != NCL_OK) {
        return false;
    }
    if (code == 250) {
        if (saved[0] != '\0') {
            ncl_ftp_client_chdir(c, saved);
        }
        return true;
    }
    return false;
}

/* --------------------------------------------------------------- transfers - */

ncl_err ncl_ftp_client_store(ncl_ftp_client *c, const char *remote,
                             const void *data, size_t len)
{
    ncl_socket *conn = NULL;
    ncl_err rc;

    if (c == NULL || remote == NULL || (data == NULL && len > 0)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    rc = ncl_ftp_client_data_command(c, "STOR", remote, &conn);
    if (rc == NCL_OK && len > 0) {
        rc = ncl_socket_send(conn, data, len);
    }
    if (conn != NULL) {
        ncl_socket_close(conn);
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    ncl_ftp_client_close_data(c);
    return rc;
}

ncl_err ncl_ftp_client_store_file(ncl_ftp_client *c, const char *remote,
                                  const char *local_path)
{
    char *data = NULL;
    size_t len = 0;
    ncl_err rc;

    rc = ncl_file_read_all(local_path, &data, &len);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_ftp_client_store(c, remote, data, len);
    ncl_mem_free(data);
    return rc;
}

ncl_err ncl_ftp_client_retrieve_file(ncl_ftp_client *c, const char *remote,
                                     const char *local_path)
{
    ncl_socket *conn = NULL;
    ncl_err rc;
    ncl_strbuf data;

    if (c == NULL || remote == NULL || local_path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    rc = ncl_ftp_client_data_command(c, "RETR", remote, &conn);
    ncl_strbuf_init(&data);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_drain(c, conn, &data);
    }
    if (conn != NULL) {
        ncl_socket_close(conn);
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    ncl_ftp_client_close_data(c);
    if (rc == NCL_OK) {
        rc = ncl_file_write_all(local_path, data.data, data.len);
    }
    ncl_strbuf_free(&data);
    return rc;
}

ncl_err ncl_ftp_client_retrieve(ncl_ftp_client *c, const char *remote,
                                ncl_strbuf *out)
{
    ncl_socket *conn = NULL;
    ncl_err rc;

    if (c == NULL || remote == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    ncl_strbuf_reset(out);
    rc = ncl_ftp_client_data_command(c, "RETR", remote, &conn);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_drain(c, conn, out);
    }
    if (conn != NULL) {
        ncl_socket_close(conn);
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    ncl_ftp_client_close_data(c);
    return rc;
}

/* ------------------------------------------------------ streamed transfers -- */

/* Offsets beyond 2 GB need the 64-bit seek of each platform. */
#if defined(NCL_OS_WINDOWS)
#  define stream_seek(fp, off) _fseeki64((fp), (off), SEEK_SET)
#else
#  define stream_seek(fp, off) fseeko((fp), (off), SEEK_SET)
#endif

/** The abort-after setting of this process; 0 when the seam is not armed. */
static long long stream_abort_after(void)
{
    const char *text = getenv(NCL_FTP_ABORT_ENV);

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    return strtoll(text, NULL, 10);
}

/** Open @p path for reading, skipping the first @p skip bytes. */
static FILE *stream_open_reader(const char *path, long long skip)
{
    FILE *fp;

    if (path == NULL) {
        return NULL;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (skip > 0 && stream_seek(fp, skip) != 0) {
        fclose(fp);
        return NULL;
    }
    return fp;
}

/** Open @p path for appending (creating it when it is absent). */
static FILE *stream_open_appender(const char *path)
{
    return path != NULL ? fopen(path, "ab") : NULL;
}

ncl_err ncl_ftp_client_upload(ncl_ftp_client *c, const char *remote,
                              const char *local_path)
{
    char buffer[NCL_FTP_STREAM_CHUNK];
    long long total;
    int attempt;
    ncl_err rc = NCL_ERR_IO;

    if (c == NULL || remote == NULL || local_path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    total = ncl_file_size(local_path);
    if (total <= 0) {
        ncl_log_error("ftp upload: cannot read %s", local_path);
        return NCL_ERR_IO;
    }
    c->abort_after = stream_abort_after();

    for (attempt = 0; attempt < NCL_FTP_TRANSFER_ATTEMPTS; attempt++) {
        ncl_socket *conn = NULL;
        FILE *fp;
        long long sent;
        long long have = 0;

        if (ncl_ftp_client_size(c, remote, &have) != NCL_OK || have > total) {
            have = 0; /* nothing there yet, or a stale file: start over */
        }
        if (have == total) {
            return NCL_OK; /* the peer already holds the whole file */
        }
        fp = stream_open_reader(local_path, have);
        if (fp == NULL) {
            return NCL_ERR_IO;
        }
        rc = ncl_ftp_client_data_command(c, have > 0 ? "APPE" : "STOR", remote,
                                         &conn);
        sent = have;
        while (rc == NCL_OK) {
            size_t got = fread(buffer, 1, sizeof(buffer), fp);
            size_t send = got;

            if (got == 0) {
                break;
            }
            if (c->abort_after > 0 && sent + (long long)got >= c->abort_after) {
                /* The resume test wants a link that dies half way; the retry
                 * below then continues from wherever the peer got to. */
                send = (size_t)(c->abort_after - sent);
                c->abort_after = 0;
                rc = NCL_ERR_CONNECT;
            }
            if (send > 0) {
                ncl_err send_rc = ncl_socket_send(conn, buffer, send);
                if (send_rc != NCL_OK) {
                    rc = send_rc;
                    break;
                }
                sent += (long long)send;
                c->bytes_sent += (long long)send;
            }
            if (rc != NCL_OK) {
                break;
            }
        }
        fclose(fp);
        if (conn != NULL) {
            ncl_socket_close(conn);
        }
        /*
         * Always take the completion reply, even when the local side already
         * gave up: the server answers 226 (it saw EOF and kept what it got) or
         * 4xx/5xx, and that line has to leave the control connection before the
         * resume sends anything else.
         */
        {
            ncl_err reply_rc = ncl_ftp_client_expect(c, 2, NULL);

            if (rc == NCL_OK) {
                rc = reply_rc;
            }
        }
        ncl_ftp_client_close_data(c);
        if (rc == NCL_OK) {
            long long landed = 0;

            if (ncl_ftp_client_size(c, remote, &landed) != NCL_OK) {
                landed = 0;
            }
            if (landed >= total) {
                return NCL_OK;
            }
            ncl_log_warn("ftp upload short (%lld/%lld bytes), resuming", landed,
                         total);
            rc = NCL_ERR_IO;
        } else {
            ncl_log_warn("ftp upload interrupted (%s), resuming",
                         ncl_err_name(rc));
        }
    }
    return rc;
}

ncl_err ncl_ftp_client_download(ncl_ftp_client *c, const char *remote,
                                const char *local_path)
{
    char buffer[NCL_FTP_STREAM_CHUNK];
    long long total = 0;
    int attempt;
    ncl_err rc = NCL_ERR_IO;

    if (c == NULL || remote == NULL || local_path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    if (ncl_ftp_client_size(c, remote, &total) != NCL_OK) {
        ncl_log_error("ftp download: no such file %s", remote);
        return NCL_ERR_NOT_FOUND;
    }
    c->abort_after = stream_abort_after();

    for (attempt = 0; attempt < NCL_FTP_TRANSFER_ATTEMPTS; attempt++) {
        ncl_socket *conn = NULL;
        FILE *fp;
        long long have = ncl_file_size(local_path);

        if (have < 0) {
            have = 0;
        }
        if (have > total) {
            remove(local_path); /* stale: longer than the peer's file */
            have = 0;
        }
        if (have == total) {
            return NCL_OK; /* already there */
        }
        if (have > 0) {
            ncl_strbuf text;

            ncl_strbuf_init(&text);
            rc = ncl_ftp_client_command(c, "REST %lld", have);
            if (rc == NCL_OK) {
                rc = ncl_ftp_client_expect(c, 3, &text);
            }
            ncl_strbuf_free(&text);
            if (rc != NCL_OK) {
                /* No REST: only a full download can be right. */
                ncl_log_warn("ftp REST %lld refused, restarting", have);
                remove(local_path);
                have = 0;
            }
        }
        fp = stream_open_appender(local_path);
        if (fp == NULL) {
            return NCL_ERR_IO;
        }
        rc = ncl_ftp_client_data_command(c, "RETR", remote, &conn);
        while (rc == NCL_OK) {
            int got = ncl_socket_recv(conn, buffer, sizeof(buffer),
                                      c->io_timeout_ms);

            if (got == NCL_SOCKET_TIMEOUT) {
                continue;
            }
            if (got < 0) {
                rc = NCL_ERR_IO;
                break;
            }
            if (got == 0) {
                break;
            }
            if (c->abort_after > 0 &&
                have + (long long)got >= c->abort_after) {
                /* Partial write, then a dead link: the retry resumes. */
                size_t keep = (size_t)(c->abort_after - have);

                c->abort_after = 0;
                if (keep > 0 && fwrite(buffer, 1, keep, fp) == keep) {
                    c->bytes_received += (long long)keep;
                }
                rc = NCL_ERR_CONNECT;
                break;
            }
            if (fwrite(buffer, 1, (size_t)got, fp) != (size_t)got) {
                rc = NCL_ERR_IO;
                break;
            }
            have += got;
            c->bytes_received += got;
        }
        fclose(fp);
        if (conn != NULL) {
            ncl_socket_close(conn);
        }
        {
            ncl_err reply_rc = ncl_ftp_client_expect(c, 2, NULL);

            if (rc == NCL_OK) {
                rc = reply_rc;
            }
        }
        ncl_ftp_client_close_data(c);
        if (rc == NCL_OK) {
            if (ncl_file_size(local_path) >= total) {
                return NCL_OK;
            }
            ncl_log_warn("ftp download short (%lld/%lld bytes), resuming",
                         ncl_file_size(local_path), total);
            rc = NCL_ERR_IO;
        } else {
            ncl_log_warn("ftp download interrupted (%s), resuming",
                         ncl_err_name(rc));
        }
    }
    return rc;
}

long long ncl_ftp_client_bytes_sent(const ncl_ftp_client *client)
{
    return client != NULL ? client->bytes_sent : 0;
}

long long ncl_ftp_client_bytes_received(const ncl_ftp_client *client)
{
    return client != NULL ? client->bytes_received : 0;
}

static ncl_err ncl_ftp_client_listing(ncl_ftp_client *c, const char *verb,
                                      const char *path, ncl_strbuf *out)
{
    ncl_socket *conn = NULL;
    ncl_err rc;

    rc = ncl_ftp_client_data_command(c, verb, path, &conn);
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_drain(c, conn, out);
    }
    if (conn != NULL) {
        ncl_socket_close(conn);
    }
    if (rc == NCL_OK) {
        rc = ncl_ftp_client_expect(c, 2, NULL);
    }
    ncl_ftp_client_close_data(c);
    return rc;
}

ncl_err ncl_ftp_client_list(ncl_ftp_client *c, const char *path,
                            ncl_ptrvec *out)
{
    ncl_strbuf raw;
    ncl_err rc;
    char *cursor;

    if (c == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    ncl_strbuf_init(&raw);
    rc = ncl_ftp_client_listing(c, "LIST", path, &raw);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&raw);
        return rc;
    }
    cursor = raw.data;
    while (cursor != NULL && *cursor != '\0') {
        char *nl = strchr(cursor, '\n');
        char *line = cursor;
        ncl_ftp_entry entry;

        if (nl != NULL) {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor = NULL;
        }
        {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
                line[--len] = '\0';
            }
        }
        if (line[0] == '\0') {
            continue;
        }
        if (ncl_ftp_parse_list_line(line, &entry) != NCL_OK) {
            continue;
        }
        {
            ncl_ftp_entry *owned =
                (ncl_ftp_entry *)ncl_mem_calloc(1, sizeof(*owned));
            if (owned == NULL) {
                ncl_ftp_entry_free(&entry);
                ncl_strbuf_free(&raw);
                return NCL_ERR_NOMEM;
            }
            *owned = entry;
            if (ncl_ptrvec_push(out, owned) != NCL_OK) {
                ncl_ftp_entry_free(owned);
                ncl_strbuf_free(&raw);
                return NCL_ERR_NOMEM;
            }
        }
    }
    ncl_strbuf_free(&raw);
    return NCL_OK;
}

ncl_err ncl_ftp_client_nlst(ncl_ftp_client *c, const char *path,
                            ncl_strvec *out)
{
    ncl_strbuf raw;
    ncl_err rc;
    char *cursor;

    if (c == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (c->ctrl == NULL) {
        return NCL_ERR_CLOSED;
    }
    ncl_strbuf_init(&raw);
    rc = ncl_ftp_client_listing(c, "NLST", path, &raw);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&raw);
        return rc;
    }
    cursor = raw.data;
    while (cursor != NULL && *cursor != '\0') {
        char *nl = strchr(cursor, '\n');
        char *line = cursor;
        size_t len;

        if (nl != NULL) {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor = NULL;
        }
        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len > 0) {
            ncl_strvec_push(out, line);
        }
    }
    ncl_strbuf_free(&raw);
    return NCL_OK;
}
