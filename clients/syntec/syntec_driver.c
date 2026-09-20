/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - SYNTEC RemoteCNC over TCP.
 *
 * The session is the connection plus a `uSerial` counter: the controller
 * echoes the serial of the request in its answer (§10.4), which is what keeps
 * a late reply from being read as the answer to the next request.
 *
 * Every exchange goes through one mutex - a control is a single session device
 * and two requests in flight would interleave their structures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/clients/syntec.h"
#include "syntec/ncl_syntec_driver.h"

#define SYNTEC_MAX_BODY 4096

typedef struct {
    char        *host;
    unsigned     port;
    unsigned     func_id;
    uint8_t      serial;
    ncl_socket  *socket;
    unsigned     connect_timeout_ms;
    unsigned     timeout_ms;
    unsigned     retries;
    ncl_mutex   *mutex;
    uint8_t      tx[SYNTEC_MAX_BODY + 32];
    uint8_t      rx[SYNTEC_MAX_BODY + 32];
    size_t       last_tx_len; /**< the frame the audit should show */
    size_t       last_rx_len;
} syntec_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void syntec_close_session(syntec_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
}

static ncl_err syntec_open_session(syntec_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    if (ctx->socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x90);
    }
    ctx->serial = 0;
    return NCL_OK;
}

/* -------------------------------------------------------------- exchange -- */

/**
 * One packet out, one packet in (§10.4): 12 bytes of header say how much
 * follows, so the reply is read in two steps like the server reads a request.
 */
static ncl_err syntec_exchange(syntec_ctx *ctx, uint16_t cmd_id,
                               uint16_t func_id, const void *body, size_t body_len,
                               ncl_syntec_view *view)
{
    ncl_syntec_function function;
    uint8_t serial = (uint8_t)++ctx->serial;
    size_t frame_len;
    size_t content;
    size_t total;
    ncl_err err;

    memset(&function, 0, sizeof(function));
    function.func_id = func_id;
    function.serial = serial;
    frame_len = ncl_syntec_build(ctx->tx, sizeof(ctx->tx), cmd_id, &function, body,
                                 body_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x91);
    }
    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, NCL_SYNTEC_PACKET_HEADER,
                              ctx->timeout_ms) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x92);
    }
    content = (size_t)ctx->rx[0] | ((size_t)ctx->rx[1] << 8) |
              ((size_t)ctx->rx[2] << 16) | ((size_t)ctx->rx[3] << 24);
    total = NCL_SYNTEC_PACKET_HEADER + content;
    if (content < NCL_SYNTEC_FUNCTION_HEADER || total > sizeof(ctx->rx)) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_PROTOCOL(0x90);
    }
    if (ncl_socket_recv_exact(ctx->socket, ctx->rx + NCL_SYNTEC_PACKET_HEADER,
                              content, ctx->timeout_ms) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x93);
    }
    ctx->last_rx_len = total;
    err = ncl_syntec_split(ctx->rx, total, view, NULL);
    if (err != NCL_OK) {
        return err;
    }
    if (view->function.serial != serial) {
        /* §10.4: the serial is echoed, so a mismatch means a stale answer. */
        return NCL_DRV_ERR_PROTOCOL(0x91);
    }
    return NCL_OK;
}

static ncl_err syntec_request(syntec_ctx *ctx, uint16_t cmd_id,
                              uint16_t func_id, const void *body, size_t body_len,
                              ncl_syntec_view *view)
{
    unsigned attempt = 0;

    for (;;) {
        ncl_err err = syntec_exchange(ctx, cmd_id, func_id, body, body_len, view);

        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        syntec_close_session(ctx);
        if (syntec_open_session(ctx) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x94);
        }
    }
}

/* ------------------------------------------------------------------ read -- */

/**
 * The answer bytes as the point's type: big endian within the field, which is
 * what the delivered client does with the doubles it reads back. The bytes
 * follow the four request fields in the reply body.
 */
static ncl_err syntec_decode(const uint8_t *data, size_t len, ncl_dtype dtype,
                             size_t count, ncl_json **out)
{
    char text[256];

    if (out != NULL) {
        *out = NULL;
    }
    if (data == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (dtype) {
    case NCL_DTYPE_BIT:
        return len < 1 ? NCL_ERR_RANGE
                       : (*out = ncl_json_new_bool(data[0] != 0), NCL_OK);
    case NCL_DTYPE_BYTE:
        return len < 1 ? NCL_ERR_RANGE
                       : (*out = ncl_json_new_int(data[0]), NCL_OK);
    case NCL_DTYPE_INT16:
        return len < 2 ? NCL_ERR_RANGE
                       : (*out = ncl_json_new_int((int16_t)((data[0] << 8) | data[1])),
                          NCL_OK);
    case NCL_DTYPE_INT32:
        return len < 4
                   ? NCL_ERR_RANGE
                   : (*out = ncl_json_new_int((int32_t)(((uint32_t)data[0] << 24) |
                                                        ((uint32_t)data[1] << 16) |
                                                        ((uint32_t)data[2] << 8) |
                                                        data[3])),
                      NCL_OK);
    case NCL_DTYPE_FLOAT32: {
        uint32_t raw;
        float value;

        if (len < 4) {
            return NCL_ERR_RANGE;
        }
        raw = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
              ((uint32_t)data[2] << 8) | data[3];
        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double((double)value);
        return NCL_OK;
    }
    case NCL_DTYPE_FLOAT64: {
        uint64_t raw = 0;
        double value;
        size_t i;

        if (len < 8) {
            return NCL_ERR_RANGE;
        }
        for (i = 0; i < 8; i++) {
            raw = (raw << 8) | data[i];
        }
        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double(value);
        return NCL_OK;
    }
    case NCL_DTYPE_STRING: {
        size_t chars = count < sizeof(text) - 1 ? count : sizeof(text) - 1;
        size_t i;

        if (chars > len) {
            chars = len;
        }
        memcpy(text, data, chars);
        text[chars] = '\0';
        for (i = 0; i < chars; i++) {
            if (text[i] == '\0') {
                text[i] = ' ';
            }
        }
        *out = ncl_json_new_string(text);
        return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
}

static ncl_err syntec_read_batch(ncl_driver *self, const ncl_address *addresses,
                                 size_t count, ncl_json **values)
{
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;
    ncl_json *array = ncl_json_new_array();
    ncl_err result = NCL_OK;
    size_t i;

    if (values == NULL || (count > 0 && addresses == NULL)) {
        ncl_json_free(array);
        return NCL_ERR_INVALID_ARG;
    }
    *values = NULL;
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    /* One packet per point: the reply carries one structure. */
    for (i = 0; i < count && result == NCL_OK; i++) {
        const ncl_address *address = &addresses[i];
        const char *canonical = NULL;
        const ncl_syntec_reading *reading;
        uint16_t cmd_id = 0;
        int32_t code;
        uint8_t body[32];
        size_t body_len;
        size_t want = address->length > 0 ? (size_t)address->length : 4u;
        ncl_syntec_view view;
        ncl_json *value = NULL;

        /* §10.12: the client's named readings are "KrnlAPI + code", so a point
         * may say "part_count" instead of naming both; the code comes from the
         * table. Otherwise the area is the command and the point's offset is
         * the dwCode. */
        code = (int32_t)address->offset;
        reading = ncl_syntec_reading_lookup(address->area);
        if (reading != NULL) {
            cmd_id = reading->cmd_id;
            code = reading->code;
        } else if (!ncl_syntec_cmd_lookup(address->area, &cmd_id, &canonical)) {
            result = NCL_DRV_ERR_BUSINESS(0xB0); /* no such command */
            break;
        }
        (void)canonical;
        /* §10.9: the controller dispatches on uFuncID and writes CmdID = uFuncID
         * into its answers, so the two fields carry the same command number
         * unless the configuration says otherwise. */
        {
            uint16_t function_id =
                ctx->func_id != 0 ? (uint16_t)ctx->func_id : cmd_id;
        /* The body is the KrnlAPI request structure (§10.2): `code` is the
         * dwCode, `length` the size asked for. */
        body_len = ncl_syntec_krnl_body(body, sizeof(body), function_id,
                                        code, 0, (int32_t)want, NULL, 0);
        if (body_len == 0) {
            result = NCL_ERR_RANGE;
            break;
        }
        memset(&view, 0, sizeof(view));
        result = syntec_request(ctx, cmd_id, function_id, body, body_len, &view);
        }
        if (result != NCL_OK) {
            break;
        }
        if (view.body_len < 14u) {
            result = NCL_DRV_ERR_PROTOCOL(0x92);
            break;
        }
        /* The reply body repeats the request fields, then carries the answer
         * bytes; the point's type says how to read them (big endian within the
         * field, which is what the delivered tooling does with the doubles). */
        result = syntec_decode(view.body + 14, view.body_len - 14, address->dtype,
                               (size_t)address->length, &value);
        if (result != NCL_OK) {
            break;
        }
        (void)ncl_json_arr_push(array, value != NULL ? value : ncl_json_new_null());
    }
    if (result != NCL_OK) {
        ncl_json_free(array);
        return result;
    }
    *values = array;
    return NCL_OK;
}

static ncl_err syntec_write_batch(ncl_driver *self, const ncl_address *addresses,
                                  const ncl_json *values, size_t count)
{
    (void)self;
    (void)addresses;
    (void)values;
    (void)count;
    return NCL_ERR_NOT_SUPPORTED;
}

static ncl_err syntec_raw(ncl_driver *self, const void *frame, size_t frame_len,
                          ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;
    const uint8_t *bytes = (const uint8_t *)frame;
    uint16_t cmd_id;
    uint16_t func_id;
    ncl_syntec_view view;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 4 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    cmd_id = (uint16_t)(bytes[0] | (bytes[1] << 8));
    func_id = (uint16_t)(bytes[2] | (bytes[3] << 8));
    memset(&view, 0, sizeof(view));
    ncl_mutex_lock(ctx->mutex);
    err = syntec_request(ctx, cmd_id, func_id, bytes + 4, frame_len - 4, &view);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    ncl_driver_result_set_raw(out, view.body, view.body_len);
    hex = (char *)ncl_mem_alloc(view.body_len * 2u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < view.body_len; i++) {
        hex[i * 2] = kDigits[(view.body[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[view.body[i] & 0xF];
    }
    hex[view.body_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

/** The session's own operations: what the control calls itself, and a ping. */
static ncl_err syntec_call(ncl_driver *self, const char *operation,
                           const ncl_json *params, ncl_json **result)
{
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "serial")) {
        if (result != NULL) {
            *result = ncl_json_new_object();
            if (*result == NULL) {
                return NCL_ERR_NOMEM;
            }
            (void)ncl_json_obj_set_int(*result, "serial", ctx->serial);
            (void)ncl_json_obj_set_int(*result, "funcId", (long long)ctx->func_id);
            (void)ncl_json_obj_set_string(*result, "host", ctx->host);
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "commandName")) {
        if (result != NULL) {
            *result = ncl_json_new_string(NCL_SYNTEC_CMD_KRML_API == 200
                                              ? "KrnlAPI"
                                              : "?");
            if (*result == NULL) {
                return NCL_ERR_NOMEM;
            }
        }
        return NCL_OK;
    }
    return NCL_DRV_ERR_PROTOCOL(0x93); /* no such operation */
}

/* ------------------------------------------------------------ the table -- */

static ncl_err syntec_create(ncl_driver *self, const ncl_json *parameters)
{
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;
    const char *text;

    if (parameters == NULL) {
        return NCL_OK;
    }
    text = ncl_json_obj_get_string(parameters, "host");
    if (text != NULL) {
        ncl_free_safe(ctx->host);
        ctx->host = ncl_strdup(text);
        if (ctx->host == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->func_id = json_uint(parameters, "funcId", ctx->func_id);
    ctx->serial = (uint8_t)json_uint(parameters, "serial", ctx->serial);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    if (ctx->func_id > 0xFFFFu) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err syntec_open(ncl_driver *self)
{
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = syntec_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void syntec_close(ncl_driver *self)
{
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    syntec_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool syntec_is_connected(const ncl_driver *self)
{
    return ((const syntec_ctx *)self->ctx)->socket != NULL;
}

static ncl_err syntec_read_batch_locked(ncl_driver *self,
                                        const ncl_address *addresses,
                                        size_t count, ncl_json **values)
{
    syntec_ctx *ctx = (syntec_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = syntec_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void syntec_attach_event(ncl_driver *self, ncl_driver_event_fn fn,
                                void *user)
{
    (void)self;
    (void)fn;
    (void)user;
}

/** The frames of the last exchange, for the audit trail (§6). */
static void syntec_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    const syntec_ctx *ctx = (const syntec_ctx *)self->ctx;

    out->request = ctx->last_tx_len > 0 ? ctx->tx : NULL;
    out->request_len = ctx->last_tx_len;
    out->reply = ctx->last_rx_len > 0 ? ctx->rx : NULL;
    out->reply_len = ctx->last_rx_len;
}

static void syntec_destroy(ncl_driver *self)
{
    syntec_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (syntec_ctx *)self->ctx;
    if (ctx != NULL) {
        syntec_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kSyntecOps = {
    "syntec",         syntec_create,
    syntec_open,      syntec_close,
    syntec_is_connected, syntec_read_batch_locked,
    syntec_write_batch, syntec_raw,
    syntec_raw,       syntec_call,
    syntec_attach_event, syntec_destroy,
    syntec_last_raw,
};

ncl_driver *ncl_syntec_create(void)
{
    syntec_ctx *ctx = (syntec_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 8000; /* §10.1: the controller's OCAPIServer listens here */
    ctx->func_id = 0; /* 0 = use the command number for both fields (§10.9) */
    ctx->serial = 1;
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 3000;
    ctx->retries = 0;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kSyntecOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
