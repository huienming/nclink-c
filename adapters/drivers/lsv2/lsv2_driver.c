/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - HEIDENHAIN LSV2 over TCP.
 *
 * A session is the connection plus, when the configuration names one, a login:
 * §4.1 grades the login names by what they allow, and §7.4 says to ask for the
 * version first, which is exactly what open() does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_lsv2.h"
#include "lsv2/ncl_lsv2_driver.h"

#define LSV2_RX_BYTES 2048

typedef struct {
    char        *host;
    unsigned     port;
    char        *user;
    char        *password;
    unsigned     count;
    ncl_socket  *socket;
    bool         logged_in;
    char         version[128]; /**< what R_VR said */
    unsigned     connect_timeout_ms;
    unsigned     timeout_ms;
    unsigned     retries;
    ncl_mutex   *mutex;
    uint8_t      tx[LSV2_RX_BYTES];
    uint8_t      rx[LSV2_RX_BYTES];
} lsv2_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void lsv2_close_session(lsv2_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
    ctx->logged_in = false;
    ctx->version[0] = '\0';
}

/** Read one frame off the socket; *payload points into the driver's buffer. */
static ncl_err lsv2_read_frame(lsv2_ctx *ctx, char name[5],
                               const uint8_t **payload, size_t *payload_len)
{
    size_t declared;
    size_t total;

    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, 8, ctx->timeout_ms) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x70);
    }
    declared = ((size_t)ctx->rx[0] << 24) | ((size_t)ctx->rx[1] << 16) |
               ((size_t)ctx->rx[2] << 8) | ctx->rx[3];
    total = 8u + declared;
    if (total > sizeof(ctx->rx)) {
        return NCL_DRV_ERR_PROTOCOL(0x70);
    }
    if (declared > 0 &&
        ncl_socket_recv_exact(ctx->socket, ctx->rx + 8, declared,
                              ctx->timeout_ms) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x71);
    }
    return ncl_lsv2_split(ctx->rx, total, name, payload, payload_len, NULL);
}

/**
 * One request/answer. The reply's name is checked against @p expect (a NULL
 * entry means "any"), and a T_ER is turned into a tiered error carrying the
 * status code the control sent.
 */
static ncl_err lsv2_exchange(lsv2_ctx *ctx, const char *command,
                             const void *payload, size_t payload_len,
                             const char *const *expect, size_t expect_count,
                             const uint8_t **reply, size_t *reply_len)
{
    size_t frame_len = ncl_lsv2_frame(ctx->tx, sizeof(ctx->tx), command, payload,
                                      payload_len);
    char name[5];
    char message[160];
    ncl_err err;
    size_t i;

    if (reply != NULL) {
        *reply = NULL;
    }
    if (reply_len != NULL) {
        *reply_len = 0;
    }
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x72);
    }
    err = lsv2_read_frame(ctx, name, reply, reply_len);
    if (err != NCL_OK) {
        return err;
    }
    if (strcmp(name, "T_ER") == 0) {
        int status = (*reply_len >= 1) ? (*reply)[0] : -1;

        message[0] = '\0';
        return ncl_lsv2_check_status(status, message, sizeof(message));
    }
    if (expect != NULL) {
        for (i = 0; i < expect_count; i++) {
            if (strcmp(name, expect[i]) == 0) {
                return NCL_OK;
            }
        }
        snprintf(message, sizeof(message), "%s 收到了 %s", command, name);
        return NCL_DRV_ERR_PROTOCOL(0x71); /* an answer to something else */
    }
    return NCL_OK;
}

static ncl_err lsv2_request(lsv2_ctx *ctx, const char *command,
                            const void *payload, size_t payload_len,
                            const char *const *expect, size_t expect_count,
                            const uint8_t **reply, size_t *reply_len)
{
    unsigned attempt = 0;

    for (;;) {
        ncl_err err = lsv2_exchange(ctx, command, payload, payload_len, expect,
                                    expect_count, reply, reply_len);

        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        /* §7.1 of 00: a machine tool session is exclusive, so a lost link is
         * rebuilt (login included) rather than written to. */
        lsv2_close_session(ctx);
        {
            char err_text[128];

            err_text[0] = '\0';
            ctx->socket = ncl_socket_connect(ctx->host, ctx->port,
                                             ctx->connect_timeout_ms, err_text,
                                             sizeof(err_text));
        }
        if (ctx->socket == NULL) {
            return NCL_DRV_ERR_TRANSPORT(0x73);
        }
    }
}

/** Ask the control what it is: §7.4 wants this first on every connection. */
static ncl_err lsv2_read_version(lsv2_ctx *ctx)
{
    static const char *const kExpect[] = {"S_VR"};
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err = lsv2_request(ctx, "R_VR", NULL, 0, kExpect, 1, &reply,
                               &reply_len);

    if (err != NCL_OK) {
        return err;
    }
    {
        size_t chars = reply_len < sizeof(ctx->version) - 1 ? reply_len
                                                            : sizeof(ctx->version) - 1;

        memcpy(ctx->version, reply, chars);
        ctx->version[chars] = '\0';
    }
    return NCL_OK;
}

/** A_LG + user + NUL [+ password + NUL] (§2). */
static ncl_err lsv2_login(lsv2_ctx *ctx)
{
    static const char *const kExpect[] = {"T_OK"};
    char payload[256];
    size_t used = 0;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;

    if (ctx->user == NULL || ctx->user[0] == '\0') {
        return NCL_OK; /* §2: some commands work without a login */
    }
    used = ncl_lsv2_string_payload(payload, sizeof(payload), ctx->user);
    if (used == 0) {
        return NCL_ERR_RANGE;
    }
    if (ctx->password != NULL && ctx->password[0] != '\0') {
        size_t more = ncl_lsv2_string_payload(payload + used,
                                              sizeof(payload) - used,
                                              ctx->password);

        if (more == 0) {
            return NCL_ERR_RANGE;
        }
        used += more;
    }
    err = lsv2_request(ctx, "A_LG", payload, used, kExpect, 1, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    ctx->logged_in = true;
    return NCL_OK;
}

static ncl_err lsv2_open_session(lsv2_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    if (ctx->socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x74);
    }
    {
        ncl_err result = lsv2_read_version(ctx);

        if (result != NCL_OK) {
            lsv2_close_session(ctx);
            return result;
        }
        result = lsv2_login(ctx);
        if (result != NCL_OK) {
            lsv2_close_session(ctx);
            return result;
        }
    }
    return NCL_OK;
}

/* ---------------------------------------------------------------- 读 ------ */

/**
 * Bytes one point asks for: the point map says the type and how many of them,
 * so an int16 at 100 is two bytes and a string of eight characters is eight -
 * the "count" parameter is only an override for the odd case.
 */
static unsigned lsv2_bytes_for(const ncl_address *address)
{
    size_t per;
    size_t count;

    switch (address->dtype) {
    case NCL_DTYPE_BIT:
    case NCL_DTYPE_BYTE:
        per = 1;
        break;
    case NCL_DTYPE_INT16:
        per = 2;
        break;
    case NCL_DTYPE_INT32:
    case NCL_DTYPE_FLOAT32:
        per = 4;
        break;
    case NCL_DTYPE_FLOAT64:
        per = 8;
        break;
    case NCL_DTYPE_STRING:
    default:
        per = 1;
        break;
    }
    count = address->dtype == NCL_DTYPE_STRING ? 1u : (size_t)address->length;
    if (address->dtype == NCL_DTYPE_STRING) {
        per = (size_t)address->length; /* one string of that many characters */
    }
    if (per * count > 0xFFu) {
        return 0xFFu; /* the R_MB length field is one byte */
    }
    return (unsigned)(per * count);
}

static ncl_err lsv2_read_one(lsv2_ctx *ctx, const ncl_address *address,
                             ncl_json **out)
{
    static const char *const kVersion[] = {"S_VR"};
    static const char *const kStatus[] = {"S_ST"};
    static const char *const kMemory[] = {"S_MB"};
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;

    if (ncl_streq_ignore_case(address->area, "version")) {
        err = lsv2_request(ctx, "R_VR", NULL, 0, kVersion, 1, &reply, &reply_len);
        if (err != NCL_OK) {
            return err;
        }
        *out = ncl_json_new_string(ctx->version);
        return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    if (ncl_streq_ignore_case(address->area, "remote_status")) {
        err = lsv2_request(ctx, "R_ST", NULL, 0, kStatus, 1, &reply, &reply_len);
        if (err != NCL_OK) {
            return err;
        }
        /* §4.3 gives no layout for S_ST, so the answer is handed over as it
         * came: the point map reads it as text (or as one byte). */
        if (address->dtype == NCL_DTYPE_STRING || reply_len > 4) {
            return ncl_lsv2_decode_memory(reply, reply_len, NCL_DTYPE_STRING,
                                          (size_t)address->length, out);
        }
        return ncl_lsv2_decode_memory(reply, reply_len,
                                      address->dtype == NCL_DTYPE_BIT
                                          ? NCL_DTYPE_BYTE
                                          : address->dtype,
                                      (size_t)address->length, out);
    }
    if (ncl_streq_ignore_case(address->area, "plc_memory")) {
        uint8_t body[8];
        unsigned count = ctx->count != 0 ? ctx->count
                                         : lsv2_bytes_for(address);
        size_t body_len = ncl_lsv2_read_memory_payload(body, sizeof(body),
                                                       address->offset, count);

        if (body_len == 0) {
            return NCL_DRV_ERR_BUSINESS(0x80); /* an address outside the range */
        }
        err = lsv2_request(ctx, "R_MB", body, body_len, kMemory, 1, &reply,
                           &reply_len);
        if (err != NCL_OK) {
            return err;
        }
        return ncl_lsv2_decode_memory(reply, reply_len, address->dtype,
                                      (size_t)address->length, out);
    }
    return NCL_DRV_ERR_BUSINESS(0x81); /* no such area in this driver */
}

static ncl_err lsv2_read_batch(ncl_driver *self, const ncl_address *addresses,
                               size_t count, ncl_json **values)
{
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;
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
    /* One transaction per point: LSV2 is a transaction protocol, and an answer
     * belongs to the command that asked for it. */
    for (i = 0; i < count && result == NCL_OK; i++) {
        ncl_json *value = NULL;

        result = lsv2_read_one(ctx, &addresses[i], &value);
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

/* ------------------------------------------------------------ the table -- */

static ncl_err lsv2_create(ncl_driver *self, const ncl_json *parameters)
{
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;
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
    text = ncl_json_obj_get_string(parameters, "user");
    if (text != NULL) {
        ncl_free_safe(ctx->user);
        ctx->user = ncl_strdup(text);
        /* §4.1: the login names are graded, so an unknown one is a mistake in
         * the configuration rather than something to send and hope. */
        if (ctx->user != NULL && ctx->user[0] != '\0' &&
            !ncl_streq_ignore_case(ctx->user, "INSPECT") &&
            !ncl_streq_ignore_case(ctx->user, "FILE") &&
            !ncl_streq_ignore_case(ctx->user, "MONITOR") &&
            !ncl_streq_ignore_case(ctx->user, "DIAGNOSTICS") &&
            !ncl_streq_ignore_case(ctx->user, "PLCDEBUG")) {
            return NCL_ERR_INVALID_ARG;
        }
    }
    text = ncl_json_obj_get_string(parameters, "password");
    if (text != NULL) {
        ncl_free_safe(ctx->password);
        ctx->password = ncl_strdup(text);
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->count = json_uint(parameters, "count", ctx->count);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err lsv2_open(ncl_driver *self)
{
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = lsv2_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void lsv2_close(ncl_driver *self)
{
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    if (ctx->socket != NULL && ctx->logged_in) {
        char payload[64];
        size_t used = ncl_lsv2_string_payload(payload, sizeof(payload),
                                              ctx->user);
        const uint8_t *reply = NULL;
        size_t reply_len = 0;

        /* A_LO is a courtesy; a control also drops the session on close. */
        (void)lsv2_exchange(ctx, "A_LO", used > 0 ? payload : NULL,
                            used > 0 ? used : 0, NULL, 0, &reply, &reply_len);
    }
    lsv2_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool lsv2_is_connected(const ncl_driver *self)
{
    return ((const lsv2_ctx *)self->ctx)->socket != NULL;
}

static ncl_err lsv2_read_batch_locked(ncl_driver *self,
                                      const ncl_address *addresses, size_t count,
                                      ncl_json **values)
{
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = lsv2_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static ncl_err lsv2_write_batch(ncl_driver *self, const ncl_address *addresses,
                                const ncl_json *values, size_t count)
{
    (void)self;
    (void)addresses;
    (void)values;
    (void)count;
    /* §7.6: C_EK and C_MC are behind a permission wall, and their payloads are
     * not in the captured material. No guessing with a machine tool. */
    return NCL_ERR_NOT_SUPPORTED;
}

/** Raw access: a four character command plus its payload, answer as it came. */
static ncl_err lsv2_raw(ncl_driver *self, const void *frame, size_t frame_len,
                        ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;
    char command[5];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 4 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memcpy(command, frame, 4);
    command[4] = '\0';
    ncl_mutex_lock(ctx->mutex);
    err = lsv2_request(ctx, command, (const uint8_t *)frame + 4, frame_len - 4,
                       NULL, 0, &reply, &reply_len);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    ncl_driver_result_set_raw(out, reply, reply_len);
    hex = (char *)ncl_mem_alloc(reply_len * 2u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < reply_len; i++) {
        hex[i * 2] = kDigits[(reply[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[reply[i] & 0xF];
    }
    hex[reply_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

/** The session's own commands: the version, a keep-alive and the login state. */
static ncl_err lsv2_call(ncl_driver *self, const char *operation,
                         const ncl_json *params, ncl_json **result)
{
    lsv2_ctx *ctx = (lsv2_ctx *)self->ctx;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "version")) {
        if (result != NULL) {
            *result = ncl_json_new_string(ctx->version);
            if (*result == NULL) {
                return NCL_ERR_NOMEM;
            }
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "keepAlive")) {
        static const char *const kExpect[] = {"S_ST"};
        const uint8_t *reply = NULL;
        size_t reply_len = 0;
        ncl_err err;

        ncl_mutex_lock(ctx->mutex);
        err = lsv2_request(ctx, "R_ST", NULL, 0, kExpect, 1, &reply, &reply_len);
        ncl_mutex_unlock(ctx->mutex);
        if (err != NCL_OK) {
            return err;
        }
        if (result != NULL) {
            *result = ncl_json_new_bool(true);
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "loginState")) {
        if (result != NULL) {
            *result = ncl_json_new_object();
            if (*result == NULL) {
                return NCL_ERR_NOMEM;
            }
            (void)ncl_json_obj_set_bool(*result, "loggedIn", ctx->logged_in);
            (void)ncl_json_obj_set_string(*result, "user",
                                          ctx->user != NULL ? ctx->user : "");
        }
        return NCL_OK;
    }
    return NCL_DRV_ERR_PROTOCOL(0x90); /* no such operation */
}

static void lsv2_attach_event(ncl_driver *self, ncl_driver_event_fn fn,
                              void *user)
{
    (void)self;
    (void)fn;
    (void)user;
}

static void lsv2_destroy(ncl_driver *self)
{
    lsv2_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (lsv2_ctx *)self->ctx;
    if (ctx != NULL) {
        lsv2_close_session(ctx);
        ncl_free_safe(ctx->host);
        ncl_free_safe(ctx->user);
        ncl_free_safe(ctx->password);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kLsv2Ops = {
    "lsv2",           lsv2_create,
    lsv2_open,        lsv2_close,
    lsv2_is_connected, lsv2_read_batch_locked,
    lsv2_write_batch, lsv2_raw,
    lsv2_raw,         lsv2_call,
    lsv2_attach_event, lsv2_destroy,
};

ncl_driver *ncl_lsv2_create(void)
{
    lsv2_ctx *ctx = (lsv2_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 19000;
    ctx->count = 0; /* 0 = take the count from each point's type and length */
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 3000;
    ctx->retries = 0;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kLsv2Ops, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
