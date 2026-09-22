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

/*
 * One session: the connection plus `uSerial`. §10.12's named readings and
 * §3.1's item service are two ways into the same connection, so both the
 * semantic API (ncl_syntec_open() + ncl_syntec_status() ...) and the generic
 * ncl_driver facade (ncl_syntec_create()) share this object.
 */
struct ncl_syntec {
    char        *host;
    unsigned     port;
    unsigned     func_id;
    uint8_t      serial;
    ncl_socket  *socket;
    unsigned     connect_timeout_ms;
    unsigned     timeout_ms;
    unsigned     retries;
    ncl_mutex   *mutex;
    char         error[160];  /**< the last failure, for the caller's log */
    uint8_t      tx[SYNTEC_MAX_BODY + 32];
    uint8_t      rx[SYNTEC_MAX_BODY + 32];
    size_t       last_tx_len; /**< the frame the audit should show */
    size_t       last_rx_len;
};

/** The driver ops carry the session; the old name keeps the diff small. */
typedef struct ncl_syntec syntec_ctx;

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
 * One ready made frame out, one packet in (§10.4): 12 bytes of header say how
 * much follows, so the reply is read in two steps like the server reads a
 * request. The frame is copied into the session buffer, which is also what the
 * audit shows.
 */
static ncl_err syntec_exchange_frame(syntec_ctx *ctx, const uint8_t *frame,
                                     size_t frame_len, uint8_t serial,
                                     ncl_syntec_view *view)
{
    size_t content;
    size_t total;
    ncl_err err;

    if (frame == NULL || frame_len == 0 || frame_len > sizeof(ctx->tx)) {
        return NCL_ERR_RANGE;
    }
    if (frame != ctx->tx) {
        memcpy(ctx->tx, frame, frame_len);
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

/** Build one packet from its parts and exchange it. */
static ncl_err syntec_exchange(syntec_ctx *ctx, uint16_t cmd_id,
                               uint16_t func_id, const void *body, size_t body_len,
                               ncl_syntec_view *view)
{
    ncl_syntec_function function;
    uint8_t serial = (uint8_t)++ctx->serial;
    size_t frame_len;

    memset(&function, 0, sizeof(function));
    function.func_id = func_id;
    function.serial = serial;
    frame_len = ncl_syntec_build(ctx->tx, sizeof(ctx->tx), cmd_id, &function, body,
                                 body_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    return syntec_exchange_frame(ctx, ctx->tx, frame_len, serial, view);
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
    if (self == NULL) {
        return;
    }
    ncl_syntec_close((ncl_syntec *)self->ctx);
    ncl_free_safe(self);
}

/** Defined with the session API below; the driver owns a session too. */
static ncl_syntec *syntec_alloc(const ncl_syntec_config *config);

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
    ncl_syntec_config config;
    ncl_syntec *ctx;
    ncl_driver *driver;

    /* The driver's parameters arrive later (the create op), so no host yet. */
    ncl_syntec_config_default(&config);
    ctx = syntec_alloc(&config);
    if (ctx == NULL) {
        return NULL;
    }
    ctx->func_id = 0; /* 0 = use the command number for both fields (§10.9) */
    driver = ncl_driver_new(&kSyntecOps, ctx);
    if (driver == NULL) {
        ncl_syntec_close(ctx);
        return NULL;
    }
    return driver;
}

/* ================================================================ session == */

void ncl_syntec_config_default(ncl_syntec_config *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->port = 8000; /* §10.1 */
    config->connect_timeout_ms = 3000;
    config->timeout_ms = 3000;
}

static ncl_syntec *syntec_alloc(const ncl_syntec_config *config)
{
    ncl_syntec *ctx = (ncl_syntec *)ncl_mem_calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = config->port != 0 ? config->port : 8000u;
    ctx->connect_timeout_ms = config->connect_timeout_ms != 0
                                  ? config->connect_timeout_ms
                                  : 3000u;
    ctx->timeout_ms = config->timeout_ms != 0 ? config->timeout_ms : 3000u;
    ctx->retries = config->retries;
    ctx->serial = 1;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    if (config->host != NULL) {
        ctx->host = ncl_strdup(config->host);
        if (ctx->host == NULL) {
            ncl_mutex_destroy(ctx->mutex);
            ncl_free_safe(ctx);
            return NULL;
        }
    }
    return ctx;
}

ncl_syntec *ncl_syntec_open(const ncl_syntec_config *config, char **err)
{
    ncl_syntec *ctx;

    if (err != NULL) {
        *err = NULL;
    }
    if (config == NULL || ncl_str_is_blank(config->host)) {
        if (err != NULL) {
            *err = ncl_strdup("新代会话需要 host");
        }
        return NULL;
    }
    ctx = syntec_alloc(config);
    if (ctx == NULL && err != NULL) {
        *err = ncl_strdup("会话分配失败（内存不足）");
    }
    return ctx;
}

void ncl_syntec_close(ncl_syntec *syntec)
{
    if (syntec == NULL) {
        return;
    }
    syntec_close_session(syntec);
    ncl_free_safe(syntec->host);
    if (syntec->mutex != NULL) {
        ncl_mutex_destroy(syntec->mutex);
    }
    ncl_free_safe(syntec);
}

bool ncl_syntec_is_open(const ncl_syntec *syntec)
{
    return syntec != NULL && syntec->socket != NULL;
}

const char *ncl_syntec_last_error(const ncl_syntec *syntec)
{
    return syntec != NULL ? syntec->error : "";
}

/** Remember one line about a failure, then hand the code back. */
static ncl_err syntec_note(ncl_syntec *syntec, ncl_err code, const char *what)
{
    snprintf(syntec->error, sizeof(syntec->error), "%s：%s", what,
             ncl_err_name(code));
    return code;
}

/** One item request: build the 36 byte frame, send it, hand the answer back. */
static ncl_err syntec_item_request(ncl_syntec *syntec, const ncl_syntec_item *item,
                                   uint32_t param_b, ncl_syntec_view *view)
{
    uint8_t frame[NCL_SYNTEC_ITEM_FRAME];
    uint8_t serial;
    ncl_err err;

    ncl_mutex_lock(syntec->mutex);
    err = syntec_open_session(syntec);
    if (err == NCL_OK) {
        serial = (uint8_t)++syntec->serial;
        if (ncl_syntec_item_frame(frame, sizeof(frame), item, param_b, serial) == 0) {
            err = NCL_ERR_RANGE;
        } else {
            err = syntec_exchange_frame(syntec, frame, sizeof(frame), serial, view);
        }
    }
    ncl_mutex_unlock(syntec->mutex);
    if (err != NCL_OK) {
        /* A dead link is the caller's business, so drop it and let the next
         * call reconnect: §3.2's per item connections are the same thing. */
        syntec_close_session(syntec);
    }
    return syntec_note(syntec, err, item != NULL ? item->name : "item");
}

/** Read one numeric item (a u16 in the answer) as a long long. */
static ncl_err syntec_item_u16(ncl_syntec *syntec, const char *name,
                               long long *value)
{
    const ncl_syntec_item *item = ncl_syntec_item_lookup(name);
    ncl_syntec_view view;
    uint16_t raw = 0;
    ncl_err err;

    if (item == NULL) {
        return NCL_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    err = syntec_item_request(syntec, item, item->param_b, &view);
    if (err != NCL_OK) {
        return err;
    }
    if (!ncl_syntec_item_u16(syntec->rx, syntec->last_rx_len, &raw)) {
        return syntec_note(syntec, NCL_ERR_RANGE, item->name);
    }
    if (value != NULL) {
        *value = (long long)raw;
    }
    return NCL_OK;
}

ncl_err ncl_syntec_part_count(ncl_syntec *syntec, long long *value)
{
    return syntec_item_u16(syntec, "PART_COUNT", value);
}

ncl_err ncl_syntec_line_number(ncl_syntec *syntec, long long *value)
{
    return syntec_item_u16(syntec, "LINE_NUMBER", value);
}

ncl_err ncl_syntec_feed_override(ncl_syntec *syntec, long long *value)
{
    return syntec_item_u16(syntec, "FEED_OVERRIDE", value);
}

ncl_err ncl_syntec_spindle_speed(ncl_syntec *syntec, long long *value)
{
    return syntec_item_u16(syntec, "SPDL_SPEED", value);
}

ncl_err ncl_syntec_spindle_override(ncl_syntec *syntec, long long *value)
{
    return syntec_item_u16(syntec, "SPDL_OVERRIDE", value);
}

ncl_err ncl_syntec_status(ncl_syntec *syntec, char *out, size_t cap)
{
    long long state = 0;
    ncl_err err;
    const char *text;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    err = syntec_item_u16(syntec, "STATUS", &state);
    if (err != NCL_OK) {
        return err;
    }
    /* §3.2: 0/1/4 空闲、2 运行、3 保持，其余未知。 */
    switch (state) {
    case 2:
        text = "running";
        break;
    case 3:
        text = "holding";
        break;
    case 0:
    case 1:
    case 4:
        text = "free";
        break;
    default:
        text = "unknown";
        break;
    }
    snprintf(out, cap, "%s", text);
    return NCL_OK;
}

ncl_err ncl_syntec_program(ncl_syntec *syntec, char *out, size_t cap)
{
    const ncl_syntec_item *item = ncl_syntec_item_lookup("PROGRAM");
    ncl_syntec_view view;
    ncl_err err;

    if (out == NULL || cap == 0 || item == NULL) {
        return item == NULL ? NCL_ERR_NOT_FOUND : NCL_ERR_INVALID_ARG;
    }
    memset(&view, 0, sizeof(view));
    err = syntec_item_request(syntec, item, item->param_b, &view);
    if (err != NCL_OK) {
        return err;
    }
    if (!ncl_syntec_item_text(syntec->rx, syntec->last_rx_len, out, cap)) {
        return syntec_note(syntec, NCL_ERR_RANGE, item->name);
    }
    return NCL_OK;
}

ncl_err ncl_syntec_feed_speed(ncl_syntec *syntec, double *value)
{
    const ncl_syntec_item *item = ncl_syntec_item_lookup("FEED_SPEED");
    ncl_syntec_view view;
    uint16_t reg = 0;
    uint16_t unit = 0;
    uint16_t mode = 0;
    ncl_err err;

    if (item == NULL || value == NULL) {
        return item == NULL ? NCL_ERR_NOT_FOUND : NCL_ERR_INVALID_ARG;
    }
    /* §3.2: register 700 first, then the states 12 and 76. */
    memset(&view, 0, sizeof(view));
    err = syntec_item_request(syntec, item, NCL_SYNTEC_FEED_SPEED_REG, &view);
    if (err != NCL_OK) {
        return err;
    }
    if (!ncl_syntec_item_u16(syntec->rx, syntec->last_rx_len, &reg)) {
        return syntec_note(syntec, NCL_ERR_RANGE, "FEED_SPEED(700)");
    }
    err = syntec_item_request(syntec, item, NCL_SYNTEC_FEED_SPEED_UNIT_STATE, &view);
    if (err != NCL_OK) {
        return err;
    }
    if (!ncl_syntec_item_u16(syntec->rx, syntec->last_rx_len, &unit)) {
        return syntec_note(syntec, NCL_ERR_RANGE, "FEED_SPEED(12)");
    }
    err = syntec_item_request(syntec, item, NCL_SYNTEC_FEED_SPEED_MODE_STATE, &view);
    if (err != NCL_OK) {
        return err;
    }
    if (!ncl_syntec_item_u16(syntec->rx, syntec->last_rx_len, &mode)) {
        return syntec_note(syntec, NCL_ERR_RANGE, "FEED_SPEED(76)");
    }
    if (mode == NCL_SYNTEC_FEED_SPEED_DIRECT) {
        *value = (double)reg;
        return NCL_OK;
    }
    /* The unit table is indexed by 状态 12: the integer step is the high bits,
     * the fractional one the low five (§3.2). Only the (0,0) entry - the factor
     * 1.0 - was measured, so anything else is honestly "还读不了" rather than a
     * guess at a table nobody captured. */
    if ((unit >> 5) == 0u && (unit & 0x1Fu) == 0u) {
        *value = (double)reg;
        return NCL_OK;
    }
    snprintf(syntec->error, sizeof(syntec->error),
             "FEED_SPEED：单位换算表待抓包（状态 12 = %u）", (unsigned)unit);
    return NCL_ERR_UNAVAILABLE;
}

ncl_err ncl_syntec_warning(ncl_syntec *syntec, ncl_json **list)
{
    const ncl_syntec_item *item = ncl_syntec_item_lookup("WARNING");
    ncl_syntec_view view;
    ncl_json *array;
    ncl_err err;

    if (item == NULL || list == NULL) {
        return item == NULL ? NCL_ERR_NOT_FOUND : NCL_ERR_INVALID_ARG;
    }
    *list = NULL;
    memset(&view, 0, sizeof(view));
    err = syntec_item_request(syntec, item, item->param_b, &view);
    if (err != NCL_OK) {
        return err;
    }
    if (!ncl_syntec_item_empty(syntec->rx, syntec->last_rx_len)) {
        /* §3.2: an empty answer is an empty list; a populated one was never
         * captured, so the layout of its entries is unknown. */
        snprintf(syntec->error, sizeof(syntec->error),
                 "WARNING：非空报警条目布局待抓包");
        return NCL_ERR_UNAVAILABLE;
    }
    array = ncl_json_new_array();
    if (array == NULL) {
        return syntec_note(syntec, NCL_ERR_NOMEM, "WARNING");
    }
    *list = array;
    return NCL_OK;
}

void ncl_syntec_last_raw(const ncl_syntec *syntec, const uint8_t **request,
                         size_t *request_len, const uint8_t **reply,
                         size_t *reply_len)
{
    if (request != NULL) {
        *request = syntec != NULL && syntec->last_tx_len > 0 ? syntec->tx : NULL;
    }
    if (request_len != NULL) {
        *request_len = syntec != NULL ? syntec->last_tx_len : 0;
    }
    if (reply != NULL) {
        *reply = syntec != NULL && syntec->last_rx_len > 0 ? syntec->rx : NULL;
    }
    if (reply_len != NULL) {
        *reply_len = syntec != NULL ? syntec->last_rx_len : 0;
    }
}
