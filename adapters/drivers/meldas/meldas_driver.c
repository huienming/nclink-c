/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Mitsubishi CNC M70/M80 over GIOP.
 *
 * The simplest session of the lot: connect, then send 80 byte requests, each
 * matched by its own request id (§2 - there is no handshake). The machine
 * allows a small number of connections, so one session is held and every
 * exchange goes through one mutex (§8.6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_meldas.h"
#include "meldas/ncl_meldas_driver.h"

#define MELDAS_MAX_REPLY 512

typedef struct {
    char         *host;
    unsigned      port;
    bool          axis_bit_mode;
    unsigned      count;
    ncl_socket   *socket;
    uint32_t      request_id;
    unsigned      connect_timeout_ms;
    unsigned      timeout_ms;
    unsigned      retries;
    ncl_mutex    *mutex;
    uint8_t       tx[NCL_MELDAS_REQUEST_BYTES];
    uint8_t       rx[MELDAS_MAX_REPLY];
} meldas_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void meldas_close_session(meldas_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
}

static ncl_err meldas_open_session(meldas_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    if (ctx->socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x30);
    }
    ctx->request_id = 0;
    return NCL_OK;
}

/* -------------------------------------------------------------- exchange -- */

/**
 * One request/answer. The reply is read in two steps: the fixed head says how
 * much more there is (the GIOP body length sits at [8]), then the data follows.
 */
static ncl_err meldas_exchange(meldas_ctx *ctx, const char *operation,
                               const ncl_meldas_request *fields,
                               ncl_meldas_value *value)
{
    uint32_t id = ++ctx->request_id;
    size_t frame_len;
    size_t body_len;
    size_t total;
    char message[160];
    ncl_err err;

    frame_len = ncl_meldas_build_request(ctx->tx, sizeof(ctx->tx), id, operation,
                                         fields);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x31);
    }
    /* 12 bytes of header, then the body length says the rest. */
    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, 12, ctx->timeout_ms) !=
        NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x32);
    }
    body_len = (size_t)ctx->rx[8] | ((size_t)ctx->rx[9] << 8) |
               ((size_t)ctx->rx[10] << 16) | ((size_t)ctx->rx[11] << 24);
    total = 12u + body_len;
    if (body_len == 0 || total > sizeof(ctx->rx)) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_PROTOCOL(0x30);
    }
    if (body_len > 0 &&
        ncl_socket_recv_exact(ctx->socket, ctx->rx + 12, body_len,
                              ctx->timeout_ms) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x33);
    }
    message[0] = '\0';
    err = ncl_meldas_parse_reply(ctx->rx, total, id, value, message,
                                 sizeof(message));
    return err;
}

static ncl_err meldas_request(meldas_ctx *ctx, const char *operation,
                              const ncl_meldas_request *fields,
                              ncl_meldas_value *value)
{
    unsigned attempt = 0;

    for (;;) {
        ncl_err err = meldas_exchange(ctx, operation, fields, value);

        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        /* §8.6: the session is exclusive, so a lost one is rebuilt rather than
         * re-sent down a half dead connection. */
        meldas_close_session(ctx);
        if (meldas_open_session(ctx) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x34);
        }
    }
}

/* ------------------------------------------------------------------ read -- */

/** The fields a point asks for: its command, and the axis / address. */
static ncl_err meldas_fields_for(const meldas_ctx *ctx, const ncl_address *address,
                                 ncl_meldas_request *fields)
{
    const char *canonical = NULL;
    uint32_t want = 0;

    memset(fields, 0, sizeof(*fields));
    if (!ncl_meldas_command_lookup(address->area, &fields->command,
                                   &fields->subcode, &want, &canonical)) {
        return NCL_DRV_ERR_BUSINESS(0x40); /* no such command in the table */
    }
    (void)canonical;
    /* The point map's type wins over the table's default when it says
     * something a MELDAS answer can be: text or a number. */
    fields->want = address->dtype == NCL_DTYPE_STRING
                       ? NCL_MELDAS_TYPE_STRING
                       : ncl_meldas_want_for(address->dtype);
    if (address->dtype == NCL_DTYPE_STRING && want == NCL_MELDAS_TYPE_STRING) {
        fields->want = NCL_MELDAS_TYPE_STRING;
    }
    /* Coordinates come back as text whatever is asked (the delivered tooling
     * reads them that way), so those commands keep the sample's choice. */
    if (want == NCL_MELDAS_TYPE_STRING) {
        fields->want = NCL_MELDAS_TYPE_STRING;
    }
    fields->count = ctx->count != 0 ? ctx->count : 1;
    fields->address = address->offset < 0 ? 0 : (uint32_t)address->offset;
    /* Only a coordinate command addresses an axis; for the others the offset is
     * an I/O point or a variable number and travels as written. */
    if (ctx->axis_bit_mode &&
        ncl_meldas_command_uses_axis(fields->command, fields->subcode)) {
        fields->address = ncl_meldas_axis((unsigned)address->offset, true);
        if (fields->address == 0) {
            return NCL_DRV_ERR_BUSINESS(0x41); /* an axis outside 1..8 */
        }
    }
    return NCL_OK;
}

static ncl_err meldas_read_batch(ncl_driver *self, const ncl_address *addresses,
                                 size_t count, ncl_json **values)
{
    meldas_ctx *ctx = (meldas_ctx *)self->ctx;
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
    /* One request per point: a MELDAS answer carries one value, and the machine
     * is the bottleneck anyway. */
    for (i = 0; i < count && result == NCL_OK; i++) {
        ncl_meldas_request fields;
        ncl_meldas_value value;
        ncl_json *json;

        result = meldas_fields_for(ctx, &addresses[i], &fields);
        if (result != NCL_OK) {
            break;
        }
        memset(&value, 0, sizeof(value));
        result = meldas_request(ctx, NCL_MELDAS_GET_DATA, &fields, &value);
        if (result != NCL_OK) {
            break;
        }
        json = ncl_meldas_value_to_json(&value, addresses[i].dtype);
        (void)ncl_json_arr_push(array, json != NULL ? json : ncl_json_new_null());
    }
    if (result != NCL_OK) {
        ncl_json_free(array);
        return result;
    }
    *values = array;
    return NCL_OK;
}

/* ------------------------------------------------------------ the table -- */

static ncl_err meldas_create(ncl_driver *self, const ncl_json *parameters)
{
    meldas_ctx *ctx = (meldas_ctx *)self->ctx;
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
    ctx->count = json_uint(parameters, "count", ctx->count);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    text = ncl_json_obj_get_string(parameters, "axisMode");
    if (text != NULL) {
        if (ncl_streq_ignore_case(text, "bit")) {
            ctx->axis_bit_mode = true;
        } else if (ncl_streq_ignore_case(text, "index")) {
            ctx->axis_bit_mode = false;
        } else {
            return NCL_ERR_INVALID_ARG;
        }
    }
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err meldas_open(ncl_driver *self)
{
    meldas_ctx *ctx = (meldas_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = meldas_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void meldas_close(ncl_driver *self)
{
    meldas_ctx *ctx = (meldas_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    meldas_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool meldas_is_connected(const ncl_driver *self)
{
    return ((const meldas_ctx *)self->ctx)->socket != NULL;
}

static ncl_err meldas_read_batch_locked(ncl_driver *self,
                                        const ncl_address *addresses,
                                        size_t count, ncl_json **values)
{
    meldas_ctx *ctx = (meldas_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = meldas_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static ncl_err meldas_write_batch(ncl_driver *self, const ncl_address *addresses,
                                  const ncl_json *values, size_t count)
{
    (void)self;
    (void)addresses;
    (void)values;
    (void)count;
    /* mochaSetData has no captured frame layout, and §8.7 wants writes off by
     * default: a machine tool parameter is not something to guess at. */
    return NCL_ERR_NOT_SUPPORTED;
}

/**
 * Raw access: the five little endian 32 bit fields of a mochaGetData call, so
 * an undocumented command can be tried on a real machine.
 */
static ncl_err meldas_raw(ncl_driver *self, const void *frame, size_t frame_len,
                          ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    meldas_ctx *ctx = (meldas_ctx *)self->ctx;
    const uint8_t *bytes = (const uint8_t *)frame;
    ncl_meldas_request fields;
    ncl_meldas_value value;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 20 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(&fields, 0, sizeof(fields));
    memset(&value, 0, sizeof(value));
    for (i = 0; i < 5; i++) {
        uint32_t field = (uint32_t)bytes[i * 4] | ((uint32_t)bytes[i * 4 + 1] << 8) |
                         ((uint32_t)bytes[i * 4 + 2] << 16) |
                         ((uint32_t)bytes[i * 4 + 3] << 24);

        switch (i) {
        case 0: fields.command = field; break;
        case 1: fields.subcode = field; break;
        case 2: fields.count = field; break;
        case 3: fields.address = field; break;
        default: fields.want = field; break;
        }
    }
    ncl_mutex_lock(ctx->mutex);
    err = meldas_request(ctx, NCL_MELDAS_GET_DATA, &fields, &value);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    /* The answer, as bytes: the type marker, then whatever it carries. */
    hex = (char *)ncl_mem_alloc(2u * 16u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    {
        uint8_t snapshot[16];
        size_t used = 0;

        snapshot[used++] = value.type;
        if (value.type == NCL_MELDAS_TYPE_STRING ||
            value.type == NCL_MELDAS_TYPE_STRING188) {
            size_t len = strlen(value.text);

            if (len > 14u) {
                len = 14u;
            }
            memcpy(snapshot + used, value.text, len);
            used += len;
        } else {
            uint64_t raw = 0;
            int k;

            if (value.type == NCL_MELDAS_TYPE_DOUBLE ||
                value.type == NCL_MELDAS_TYPE_DOUBLE10) {
                memcpy(&raw, &value.real, sizeof(raw));
            } else {
                raw = (uint64_t)value.integer;
            }
            for (k = 0; k < 8; k++) {
                snapshot[used++] = (uint8_t)(raw >> (8 * k));
            }
        }
        for (i = 0; i < used; i++) {
            hex[i * 2] = kDigits[(snapshot[i] >> 4) & 0xF];
            hex[i * 2 + 1] = kDigits[snapshot[i] & 0xF];
        }
        hex[used * 2] = '\0';
    }
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

static void meldas_attach_event(ncl_driver *self, ncl_driver_event_fn fn,
                                void *user)
{
    (void)self;
    (void)fn;
    (void)user;
}

static void meldas_destroy(ncl_driver *self)
{
    meldas_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (meldas_ctx *)self->ctx;
    if (ctx != NULL) {
        meldas_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kMeldasOps = {
    "meldas",         meldas_create,
    meldas_open,      meldas_close,
    meldas_is_connected, meldas_read_batch_locked,
    meldas_write_batch, meldas_raw,
    meldas_raw,       NULL,
    meldas_attach_event, meldas_destroy,
};

ncl_driver *ncl_meldas_create(void)
{
    meldas_ctx *ctx = (meldas_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 683; /* §1: machine parameter #1929 can move it */
    ctx->axis_bit_mode = true;
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 1000;
    ctx->retries = 0;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kMeldasOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
