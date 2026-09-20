/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the KND driver (see ncl_knd_driver.h).
 *
 * No codec, because there is no frame: the controller answers one JSON object
 * per endpoint, so the work is table lookup + value shaping. The shape rules
 * come from the delivered mapping layer (`lua_mod/knd_mod.lua`), which is the
 * implementation that runs in the field.
 */

#include "nclink/clients/knd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_logger.h"
#include "http/ncl_http_client.h"
#include "knd/ncl_knd_driver.h"

typedef struct {
    char        *host;
    char        *user;
    char        *password;
    unsigned     port;
    unsigned     timeout_ms;
    bool         connected;
    ncl_mutex   *mutex;
    /* One endpoint may carry several items; a batch fetches it once. */
    const char  *cache_path;
    ncl_json    *cache_doc;
} knd_ctx;

/** Round to the nearest integer without pulling in libm (the library has no
 *  external dependencies on purpose). */
static long long round_to_int(double value)
{
    return (long long)(value < 0.0 ? value - 0.5 : value + 0.5);
}

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

/** One GET of @p path; the parsed document, or a tiered error. */
static ncl_err knd_fetch(knd_ctx *ctx, const char *path, ncl_json **document,
                         char *message, size_t message_len)
{
    ncl_http_request request;
    char *body = NULL;
    size_t body_len = 0;
    ncl_strbuf err;
    ncl_err result;

    memset(&request, 0, sizeof(request));
    request.host = ctx->host;
    request.port = ctx->port;
    request.path = path;
    request.user = ctx->user;
    request.password = ctx->password;
    request.timeout_ms = ctx->timeout_ms;
    result = ncl_http_get(&request, &body, &body_len, message, message_len);
    if (result != NCL_OK) {
        return result;
    }
    ncl_strbuf_init(&err);
    *document = ncl_json_parse_cstr(body != NULL ? body : "", &err);
    if (*document == NULL) {
        if (message != NULL) {
            snprintf(message, message_len, "%s 的回复不是 JSON: %s", path,
                     err.len > 0 ? ncl_strbuf_cstr(&err) : "(未知)");
        }
        ncl_strbuf_free(&err);
        ncl_free_safe(body);
        return NCL_DRV_ERR_PROTOCOL(0xA0);
    }
    ncl_strbuf_free(&err);
    ncl_free_safe(body);
    return NCL_OK;
}

/** The document for @p path, from the batch cache when it is still fresh. */
static ncl_err knd_document(knd_ctx *ctx, const char *path, ncl_json **document,
                            char *message, size_t message_len)
{
    if (ctx->cache_path == path && ctx->cache_doc != NULL) {
        *document = ctx->cache_doc;
        return NCL_OK;
    }
    if (ctx->cache_doc != NULL) {
        ncl_json_free(ctx->cache_doc);
        ctx->cache_doc = NULL;
        ctx->cache_path = NULL;
    }
    {
        ncl_json *fresh = NULL;
        ncl_err result = knd_fetch(ctx, path, &fresh, message, message_len);

        if (result != NCL_OK) {
            return result;
        }
        ctx->cache_path = path;
        ctx->cache_doc = fresh;
        *document = fresh;
    }
    return NCL_OK;
}

/**
 * The session is a probe: `/status` is the cheapest endpoint the controller
 * always has (the delivered layer reads it for the ready bit). It also makes a
 * wrong host visible at open time instead of at the first point.
 */
static ncl_err knd_open(ncl_driver *self)
{
    knd_ctx *ctx = (knd_ctx *)self->ctx;
    ncl_json *document = NULL;
    char message[256];

    ncl_mutex_lock(ctx->mutex);
    if (ctx->connected) {
        ncl_mutex_unlock(ctx->mutex);
        return NCL_OK;
    }
    message[0] = '\0';
    {
        ncl_err result = knd_fetch(ctx, "/status", &document, message,
                                   sizeof(message));

        ncl_json_free(document);
        if (result != NCL_OK) {
            ncl_mutex_unlock(ctx->mutex);
            if (message[0] != '\0') {
                ncl_log_warn("KND 连接失败: %s", message);
            }
            return result;
        }
    }
    ctx->connected = true;
    ncl_mutex_unlock(ctx->mutex);
    return NCL_OK;
}

static void knd_close(ncl_driver *self)
{
    knd_ctx *ctx = (knd_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    ctx->connected = false;
    ncl_json_free(ctx->cache_doc);
    ctx->cache_doc = NULL;
    ctx->cache_path = NULL;
    ncl_mutex_unlock(ctx->mutex);
}

static bool knd_is_connected(const ncl_driver *self)
{
    const knd_ctx *ctx = (const knd_ctx *)self->ctx;

    return ctx->connected;
}

/* ----------------------------------------------------------------- values -- */

/** A number out of @p document under @p field, or false when it is not one. */
static bool json_number(const ncl_json *document, const char *field,
                        double *out)
{
    const ncl_json *value = ncl_json_obj_get(document, field);

    return value != NULL && ncl_json_as_double(value, out);
}

static ncl_err knd_shape_value(const ncl_knd_item *item, const ncl_json *doc,
                               ncl_json **out, char *message, size_t msg_len)
{
    double number = 0;

    switch (item->shape) {
    case NCL_KND_ALARMS:
        /* The document is {报警类别: 文本}: report the ones that have text,
         * numbered the way the delivered layer numbers them (100%02d). */
        {
            ncl_json *list = ncl_json_new_array();
            size_t i;

            if (list == NULL) {
                return NCL_ERR_NOMEM;
            }
            for (i = 0; i < ncl_knd_alarm_class_count(); i++) {
                const char *text = ncl_json_obj_get_string(
                    doc, ncl_knd_alarm_class(i));

                if (ncl_str_is_blank(text)) {
                    continue;
                }
                {
                    ncl_json *entry = ncl_json_new_object();
                    char alarm_no[8];

                    if (entry == NULL) {
                        ncl_json_free(list);
                        return NCL_ERR_NOMEM;
                    }
                    (void)snprintf(alarm_no, sizeof(alarm_no), "100%02u",
                                   (unsigned)(i + 1));
                    if (ncl_json_obj_set_string(entry, "number", alarm_no) !=
                            NCL_OK ||
                        ncl_json_obj_set_string(entry, "text", text) != NCL_OK ||
                        ncl_json_arr_push(list, entry) != NCL_OK) {
                        ncl_json_free(entry);
                        ncl_json_free(list);
                        return NCL_ERR_NOMEM;
                    }
                }
            }
            *out = list;
        }
        return NCL_OK;
    case NCL_KND_FIRST:
        {
            const ncl_json *first = ncl_json_arr_get(doc, 0);

            if (first == NULL) {
                snprintf(message, msg_len, "%s 的元素 0 是空的", item->item);
                return NCL_DRV_ERR_BUSINESS(0xB1);
            }
            *out = ncl_json_clone(first);
            return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
        }
    case NCL_KND_AXIS:
        {
            char field[2];

            field[0] = NCL_KND_AXIS_LETTERS[item->axis];
            field[1] = '\0';
            if (!json_number(doc, field, &number)) {
                snprintf(message, msg_len, "%s 里没有轴 %s", item->item, field);
                return NCL_DRV_ERR_BUSINESS(0xB1);
            }
            *out = ncl_json_new_double(number);
            return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
        }
    default:
        break;
    }

    if (!json_number(doc, item->field, &number)) {
        snprintf(message, msg_len, "%s 里没有 %s", item->item, item->field);
        return NCL_DRV_ERR_BUSINESS(0xB1);
    }
    switch (item->shape) {
    case NCL_KND_INTEGER:
        *out = ncl_json_new_int(round_to_int(number));
        break;
    case NCL_KND_TEXT:
        {
            char text[32];

            (void)snprintf(text, sizeof(text), "%lld",
                           round_to_int(number));
            *out = ncl_json_new_string(text);
        }
        break;
    case NCL_KND_PERCENT:
        *out = ncl_json_new_double(number * 100.0);
        break;
    case NCL_KND_BIT0:
        *out = ncl_json_new_bool(((long long)number & 0x1) != 0);
        break;
    case NCL_KND_RUN_STATUS:
        switch ((int)number) {
        case 0: *out = ncl_json_new_string("free"); break;
        case 1: *out = ncl_json_new_string("holding"); break;
        case 2: *out = ncl_json_new_string("running"); break;
        default:
            snprintf(message, msg_len, "%s 的运行状态码 %d 不认识", item->item,
                     (int)number);
            return NCL_DRV_ERR_BUSINESS(0xB2);
        }
        break;
    default:
        *out = ncl_json_new_double(number);
        break;
    }
    return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/* ------------------------------------------------------------- the table -- */

static ncl_err knd_read_batch(ncl_driver *self, const ncl_address *addresses,
                              size_t count, ncl_json **values)
{
    knd_ctx *ctx = (knd_ctx *)self->ctx;
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
    ncl_mutex_lock(ctx->mutex);
    for (i = 0; i < count && result == NCL_OK; i++) {
        const ncl_knd_item *item = ncl_knd_item_lookup(addresses[i].area);
        ncl_json *document = NULL;
        ncl_json *value = NULL;
        char message[192];

        if (item == NULL) {
            result = NCL_DRV_ERR_BUSINESS(0xB0); /* no such item */
            break;
        }
        message[0] = '\0';
        result = knd_document(ctx, item->path, &document, message,
                              sizeof(message));
        if (result != NCL_OK) {
            break;
        }
        result = knd_shape_value(item, document, &value, message,
                                 sizeof(message));
        if (result != NCL_OK) {
            ncl_log_warn("KND 读 %s 失败: %s", item->item, message);
            break;
        }
        if (ncl_json_arr_push(array, value) != NCL_OK) {
            ncl_json_free(value);
            result = NCL_ERR_NOMEM;
            break;
        }
    }
    ncl_json_free(ctx->cache_doc);
    ctx->cache_doc = NULL;
    ctx->cache_path = NULL;
    ncl_mutex_unlock(ctx->mutex);
    if (result != NCL_OK) {
        ncl_json_free(array);
        return result;
    }
    *values = array;
    return NCL_OK;
}

/** Read only: the delivered mapping layer registers no write endpoint. */
static ncl_err knd_write_batch(ncl_driver *self, const ncl_address *addresses,
                               const ncl_json *values, size_t count)
{
    (void)self;
    (void)addresses;
    (void)values;
    (void)count;
    return NCL_ERR_NOT_SUPPORTED;
}

static ncl_err knd_create(ncl_driver *self, const ncl_json *parameters)
{
    knd_ctx *ctx = (knd_ctx *)self->ctx;
    const char *text;

    if (parameters == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    text = ncl_json_obj_get_string(parameters, "host");
    if (text == NULL) {
        text = ncl_json_obj_get_string(parameters, "ipAddress");
    }
    if (text != NULL) {
        ncl_free_safe(ctx->host);
        ctx->host = ncl_strdup(text);
    }
    text = ncl_json_obj_get_string(parameters, "user");
    if (text != NULL) {
        ncl_free_safe(ctx->user);
        ctx->user = ncl_strdup(text);
    }
    text = ncl_json_obj_get_string(parameters, "password");
    if (text != NULL) {
        ncl_free_safe(ctx->password);
        ctx->password = ncl_strdup(text);
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static void knd_destroy(ncl_driver *self)
{
    knd_ctx *ctx = (knd_ctx *)self->ctx;

    if (ctx != NULL) {
        ncl_json_free(ctx->cache_doc);
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

/** The ops in order: protocol, create, open, close, is_connected, read_batch,
 *  write_batch, read_raw, write_raw, call, attach_event, destroy, last_raw. */
static const ncl_driver_ops kKndOps = {
    "knd",            knd_create,
    knd_open,         knd_close,
    knd_is_connected, knd_read_batch,
    knd_write_batch,  NULL,
    NULL,             NULL,
    NULL,             knd_destroy,
    NULL, /* no frame of its own: the raw bytes would be an HTTP document */
};

ncl_driver *ncl_knd_create(void)
{
    knd_ctx *ctx = (knd_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 80;
    ctx->timeout_ms = 3000;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kKndOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
