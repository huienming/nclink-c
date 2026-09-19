/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the HTTP GET an MTConnect agent answers (see
 * ncl_mtconnect.h). A minimal client: one request per call, no keep-alive,
 * and the three body shapes an agent may use - Content-Length, chunked, or
 * read until the connection closes.
 */

#include "nclink_adapter/ncl_mtconnect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"

#define MT_MAX_BODY (16u * 1024u * 1024u)

/** Base64, for the optional Basic authentication. */
static void base64_encode(const char *text, size_t len, char *out, size_t out_len)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    size_t written = 0;

    while (at + 2 < len && written + 4 < out_len) {
        unsigned value = ((unsigned char)text[at] << 16) |
                         ((unsigned char)text[at + 1] << 8) |
                         (unsigned char)text[at + 2];

        out[written++] = kAlphabet[(value >> 18) & 0x3F];
        out[written++] = kAlphabet[(value >> 12) & 0x3F];
        out[written++] = kAlphabet[(value >> 6) & 0x3F];
        out[written++] = kAlphabet[value & 0x3F];
        at += 3;
    }
    if (at < len && written + 4 < out_len) {
        unsigned value = (unsigned char)text[at] << 16;

        out[written++] = kAlphabet[(value >> 18) & 0x3F];
        if (at + 1 < len) {
            value |= (unsigned char)text[at + 1] << 8;
            out[written++] = kAlphabet[(value >> 12) & 0x3F];
            out[written++] = kAlphabet[(value >> 6) & 0x3F];
        } else {
            out[written++] = kAlphabet[(value >> 12) & 0x3F];
            out[written++] = '=';
        }
        out[written++] = '=';
    }
    out[written] = '\0';
}

/** Read everything up to the end of the headers into @p buffer. */
static ncl_err read_headers(ncl_socket *socket, ncl_strbuf *buffer,
                            unsigned timeout_ms)
{
    char chunk[1024];

    while (buffer->len < 65536) {
        int got = ncl_socket_recv(socket, chunk, sizeof(chunk), timeout_ms);

        if (got == NCL_SOCKET_TIMEOUT || got == 0) {
            return NCL_DRV_ERR_TRANSPORT(0x80);
        }
        if (got < 0) {
            return NCL_DRV_ERR_TRANSPORT(0x81);
        }
        if (ncl_strbuf_append(buffer, chunk, (size_t)got) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
        if (strstr(ncl_strbuf_cstr(buffer), "\r\n\r\n") != NULL) {
            return NCL_OK;
        }
    }
    return NCL_DRV_ERR_PROTOCOL(0x80); /* a header block that never ends */
}

/** Case insensitive "name:" lookup in a header block; returns the value. */
static const char *header_value(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    const char *at = headers;

    while (at != NULL && *at != '\0') {
        if (ncl_strncasecmp(at, name, name_len) == 0 && at[name_len] == ':') {
            const char *value = at + name_len + 1;

            while (*value == ' ' || *value == '\t') {
                value++;
            }
            return value;
        }
        at = strchr(at, '\n');
        if (at != NULL) {
            at++;
        }
    }
    return NULL;
}

/** Read exactly @p want bytes of body into @p body. */
static ncl_err read_exact_body(ncl_socket *socket, ncl_strbuf *pending,
                               size_t want, ncl_strbuf *body,
                               unsigned timeout_ms)
{
    char chunk[2048];

    while (body->len < want) {
        size_t need = want - body->len;
        int got;

        if (pending->len > 0) {
            size_t take = pending->len < need ? pending->len : need;

            if (ncl_strbuf_append(body, pending->data, take) != NCL_OK) {
                return NCL_ERR_NOMEM;
            }
            memmove(pending->data, pending->data + take, pending->len - take);
            pending->len -= take;
            pending->data[pending->len] = '\0';
            continue;
        }
        got = ncl_socket_recv(socket, chunk, need < sizeof(chunk) ? need
                                                                 : sizeof(chunk),
                              timeout_ms);
        if (got == NCL_SOCKET_TIMEOUT || got == 0) {
            return NCL_DRV_ERR_TRANSPORT(0x82);
        }
        if (got < 0 || ncl_strbuf_append(body, chunk, (size_t)got) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x83);
        }
    }
    return NCL_OK;
}

/** Read a chunked body: size lines in hex, then the chunk, then CRLF. */
static ncl_err read_chunked_body(ncl_socket *socket, ncl_strbuf *pending,
                                 ncl_strbuf *body, unsigned timeout_ms)
{
    for (;;) {
        const char *line_end;
        unsigned long size = 0;

        /* The size line may still be arriving. */
        while ((line_end = strstr(ncl_strbuf_cstr(pending), "\r\n")) == NULL) {
            char chunk[1024];
            int got = ncl_socket_recv(socket, chunk, sizeof(chunk), timeout_ms);

            if (got <= 0) {
                return NCL_DRV_ERR_TRANSPORT(0x84);
            }
            if (ncl_strbuf_append(pending, chunk, (size_t)got) != NCL_OK) {
                return NCL_ERR_NOMEM;
            }
        }
        {
            const char *at = pending->data;
            bool hex = true;

            while (*at != '\0' && at < line_end) {
                char c = *at++;

                if (c == ';') {
                    break; /* a chunk extension */
                }
                if (c >= '0' && c <= '9') {
                    size = size * 16 + (unsigned long)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    size = size * 16 + (unsigned long)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    size = size * 16 + (unsigned long)(c - 'A' + 10);
                } else {
                    hex = false;
                    break;
                }
            }
            if (!hex) {
                return NCL_DRV_ERR_PROTOCOL(0x81);
            }
        }
        /* Drop the size line. */
        {
            size_t used = (size_t)(line_end - pending->data) + 2u;

            memmove(pending->data, pending->data + used, pending->len - used);
            pending->len -= used;
            pending->data[pending->len] = '\0';
        }
        if (size == 0) {
            return NCL_OK; /* the terminating chunk */
        }
        {
            size_t before = body->len;

            ncl_strbuf *target = body;

            if (read_exact_body(socket, pending, size, target, timeout_ms) !=
                NCL_OK) {
                return NCL_DRV_ERR_TRANSPORT(0x85);
            }
            if (body->len - before != size) {
                return NCL_DRV_ERR_TRANSPORT(0x86);
            }
        }
        /* Skip the CRLF that closes the chunk. */
        while (pending->len < 2) {
            char chunk[64];
            int got = ncl_socket_recv(socket, chunk, sizeof(chunk), timeout_ms);

            if (got <= 0) {
                return NCL_DRV_ERR_TRANSPORT(0x87);
            }
            if (ncl_strbuf_append(pending, chunk, (size_t)got) != NCL_OK) {
                return NCL_ERR_NOMEM;
            }
        }
        memmove(pending->data, pending->data + 2, pending->len - 2);
        pending->len -= 2;
        pending->data[pending->len] = '\0';
    }
}

ncl_err ncl_mtconnect_get(const ncl_mtconnect_http *request, char **body,
                          size_t *body_len, char *err, size_t err_len)
{
    ncl_socket *socket;
    ncl_strbuf text;
    ncl_strbuf pending;
    ncl_strbuf out;
    ncl_err result = NCL_OK;
    unsigned port;
    unsigned timeout;
    char message[512];
    const char *headers;
    const char *body_at;
    const char *status;
    const char *length;
    const char *encoding;

    if (body != NULL) {
        *body = NULL;
    }
    if (body_len != NULL) {
        *body_len = 0;
    }
    if (request == NULL || request->host == NULL || request->path == NULL ||
        body == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    port = request->port != 0 ? request->port : 7878;
    timeout = request->timeout_ms != 0 ? request->timeout_ms : 3000;

    socket = ncl_socket_connect(request->host, port, timeout, message,
                                sizeof(message));
    if (socket == NULL) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot reach %s:%u (%s)", request->host, port,
                     message);
        }
        return NCL_DRV_ERR_TRANSPORT(0x88);
    }
    ncl_strbuf_init(&text);
    ncl_strbuf_init(&pending);
    ncl_strbuf_init(&out);
    (void)ncl_strbuf_printf(&text, "GET %s HTTP/1.1\r\nHost: %s:%u\r\n"
                                   "User-Agent: nclink-adapter\r\n"
                                   "Accept: application/xml\r\n"
                                   "Connection: close\r\n",
                            request->path, request->host, port);
    if (request->user != NULL && request->user[0] != '\0') {
        char credentials[512];
        char encoded[700];

        snprintf(credentials, sizeof(credentials), "%s:%s", request->user,
                 request->password != NULL ? request->password : "");
        base64_encode(credentials, strlen(credentials), encoded, sizeof(encoded));
        (void)ncl_strbuf_printf(&text, "Authorization: Basic %s\r\n", encoded);
    }
    (void)ncl_strbuf_puts(&text, "\r\n");

    if (ncl_socket_send(socket, text.data, text.len) != NCL_OK) {
        result = NCL_DRV_ERR_TRANSPORT(0x89);
        goto done;
    }
    result = read_headers(socket, &pending, timeout);
    if (result != NCL_OK) {
        goto done;
    }
    headers = ncl_strbuf_cstr(&pending);
    status = strchr(headers, ' ');
    if (status == NULL || status[1] != '2') {
        if (err != NULL) {
            snprintf(err, err_len, "the agent answered %s",
                     status != NULL ? status + 1 : "nothing usable");
        }
        result = NCL_DRV_ERR_PROTOCOL(0x82);
        goto done;
    }
    body_at = strstr(headers, "\r\n\r\n");
    if (body_at == NULL) {
        result = NCL_DRV_ERR_PROTOCOL(0x83);
        goto done;
    }
    body_at += 4;
    /* Everything after the blank line is body so far. */
    {
        size_t header_bytes = (size_t)(body_at - headers);
        size_t carry = pending.len - header_bytes;

        if (carry > 0 && ncl_strbuf_append(&out, body_at, carry) != NCL_OK) {
            result = NCL_ERR_NOMEM;
            goto done;
        }
        pending.len = 0;
        if (pending.data != NULL) {
            pending.data[0] = '\0';
        }
    }
    length = header_value(headers, "Content-Length");
    encoding = header_value(headers, "Transfer-Encoding");
    if (encoding != NULL && ncl_strncasecmp(encoding, "chunked", 7) == 0) {
        result = read_chunked_body(socket, &pending, &out, timeout);
    } else if (length != NULL) {
        size_t want = (size_t)strtoul(length, NULL, 10);

        if (want > MT_MAX_BODY * 4) {
            result = NCL_DRV_ERR_PROTOCOL(0x84);
        } else {
            result = read_exact_body(socket, &pending, want, &out, timeout);
        }
    } else {
        /* No length: read until the agent closes, which "Connection: close"
         * makes the normal case. */
        for (;;) {
            char chunk[2048];
            int got = ncl_socket_recv(socket, chunk, sizeof(chunk), timeout);

            if (got == NCL_SOCKET_TIMEOUT) {
                break;
            }
            if (got <= 0) {
                break;
            }
            if (out.len + (size_t)got > MT_MAX_BODY ||
                ncl_strbuf_append(&out, chunk, (size_t)got) != NCL_OK) {
                result = NCL_ERR_NOMEM;
                break;
            }
        }
    }
    if (result == NCL_OK) {
        if (out.len > MT_MAX_BODY) {
            result = NCL_DRV_ERR_PROTOCOL(0x85);
        } else {
            *body = ncl_strbuf_detach(&out);
            if (body_len != NULL) {
                *body_len = *body != NULL ? strlen(*body) : 0;
            }
            if (*body == NULL) {
                result = NCL_ERR_NOMEM;
            }
        }
    }

done:
    ncl_strbuf_free(&text);
    ncl_strbuf_free(&pending);
    ncl_strbuf_free(&out);
    ncl_socket_close(socket);
    return result;
}
