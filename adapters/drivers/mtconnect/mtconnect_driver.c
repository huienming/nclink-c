/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the MTConnect driver.
 *
 * One HTTP GET per read: /current carries the values, /probe carries what the
 * agent knows about them. The address of a point is the dataItemId itself (see
 * the header), so a read is a table lookup rather than a protocol exchange.
 *
 * Two things the spec is explicit about are handled here rather than left to
 * the caller: an UNAVAILABLE item becomes JSON null instead of a fake zero
 * (§6.1), and a CONDITION that turns into Fault or Warning is reported as an
 * event once, not on every poll (§6.6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink_adapter/ncl_mtconnect.h"
#include "http/ncl_http_client.h"
#include "mtconnect/ncl_mtconnect_driver.h"

#define MT_MAX_STATES 64

typedef struct {
    char *id;
    char *state;
} mt_state;

typedef struct {
    char             *host;
    unsigned          port;
    char             *base_path;
    char             *user;
    char             *password;
    unsigned          timeout_ms;
    ncl_mt_probe      probe;
    bool              probe_absent; /**< the agent has no readable /probe */
    long long         last_sequence;
    mt_state          states[MT_MAX_STATES];
    size_t            state_count;
    ncl_mutex        *mutex;
    ncl_driver_event_fn event_fn;
    void             *event_user;
} mt_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/** Build the request path: the base prefix plus the document. */
static char *mt_path(const mt_ctx *ctx, const char *document)
{
    ncl_strbuf buffer;
    char *out;
    size_t len;

    if (ncl_str_is_empty(ctx->base_path)) {
        return ncl_strdup(document);
    }
    ncl_strbuf_init(&buffer);
    (void)ncl_strbuf_puts(&buffer, ctx->base_path);
    len = strlen(ctx->base_path);
    if (len > 0 && ctx->base_path[len - 1] != '/') {
        (void)ncl_strbuf_putc(&buffer, '/');
    }
    {
        const char *rest = document;

        while (*rest == '/') {
            rest++;
        }
        (void)ncl_strbuf_puts(&buffer, rest);
    }
    out = ncl_strbuf_detach(&buffer);
    ncl_strbuf_free(&buffer);
    return out;
}

/** A JSON number out of a text value; false when the text is not one. */
static bool text_to_number(const char *text, double *out)
{
    char *end = NULL;
    double value;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    value = strtod(text, &end);
    if (end == text || (end != NULL && *end != '\0')) {
        return false;
    }
    *out = value;
    return true;
}

static ncl_err mt_get(mt_ctx *ctx, const char *document, char **body,
                      size_t *body_len, char *err, size_t err_len)
{
    ncl_http_request request;
    char *path = mt_path(ctx, document);
    ncl_err result;

    if (path == NULL) {
        return NCL_ERR_NOMEM;
    }
    memset(&request, 0, sizeof(request));
    request.host = ctx->host;
    request.port = ctx->port;
    request.path = path;
    request.user = ctx->user;
    request.password = ctx->password;
    request.timeout_ms = ctx->timeout_ms;
    result = ncl_http_get(&request, body, body_len, err, err_len);
    ncl_free_safe(path);
    return result;
}

/* ------------------------------------------------------------- conditions -- */

/** True for the two states worth an event: Fault and Warning (§6.6). */
static bool state_is_alarming(const char *state)
{
    return state != NULL && (ncl_streq_ignore_case(state, "Fault") ||
                             ncl_streq_ignore_case(state, "Warning") ||
                             ncl_streq_ignore_case(state, "FAULT") ||
                             ncl_streq_ignore_case(state, "WARNING"));
}

/**
 * Remember the last state of a condition and report a change into an alarming
 * state once, so a poll loop does not repeat the same alarm. The first sight of
 * a Fault counts as a change - an alarm nobody was told about is exactly what a
 * poll loop must not swallow.
 */
static void mt_push_alarm(mt_ctx *ctx, const ncl_mt_reading *reading)
{
    ncl_json *event;

    if (ctx->event_fn == NULL) {
        return;
    }
    event = ncl_json_new_object();
    if (event == NULL) {
        return;
    }
    (void)ncl_json_obj_set_string(event, "key", reading->item_id);
    (void)ncl_json_obj_set_string(event, "value", reading->value);
    if (reading->severity != NULL) {
        (void)ncl_json_obj_set_string(event, "severity", reading->severity);
    }
    if (reading->native_code != NULL) {
        (void)ncl_json_obj_set_string(event, "code", reading->native_code);
    }
    ctx->event_fn(ctx->event_user, reading->item_id, event);
    ncl_json_free(event);
}

static void mt_note_condition(mt_ctx *ctx, const ncl_mt_reading *reading)
{
    size_t i;

    for (i = 0; i < ctx->state_count; i++) {
        if (strcmp(ctx->states[i].id, reading->item_id) == 0) {
            if (ncl_streq_ignore_case(ctx->states[i].state, reading->value)) {
                return; /* nothing changed */
            }
            ncl_free_safe(ctx->states[i].state);
            ctx->states[i].state = ncl_strdup(reading->value);
            if (state_is_alarming(reading->value)) {
                mt_push_alarm(ctx, reading);
            }
            return;
        }
    }
    if (ctx->state_count >= MT_MAX_STATES) {
        return; /* a bounded table: the first conditions seen win */
    }
    ctx->states[ctx->state_count].id = ncl_strdup(reading->item_id);
    ctx->states[ctx->state_count].state = ncl_strdup(reading->value);
    ctx->state_count++;
    if (state_is_alarming(reading->value)) {
        mt_push_alarm(ctx, reading);
    }
}

/* ------------------------------------------------------------------ read -- */

/**
 * The reading of @p id as JSON. Numeric types take the number when the text is
 * one; otherwise the text itself comes back, because MTConnect values are
 * heterogeneous by design (an EXECUTION is a word, a POSITION a number).
 */
static ncl_json *mt_reading_to_json(const ncl_mt_reading *reading,
                                    ncl_dtype dtype)
{
    double number = 0;

    if (ncl_mt_value_is_unavailable(reading->value)) {
        return ncl_json_new_null(); /* §6.1: no value, not a fake zero */
    }
    if (dtype == NCL_DTYPE_STRING) {
        return ncl_json_new_string(reading->value);
    }
    if (dtype == NCL_DTYPE_BIT) {
        return ncl_json_new_bool(ncl_streq_ignore_case(reading->value, "true") ||
                                 ncl_streq_ignore_case(reading->value, "on") ||
                                 ncl_streq_ignore_case(reading->value, "active") ||
                                 ncl_streq_ignore_case(reading->value, "normal") ||
                                 strcmp(reading->value, "1") == 0);
    }
    if (text_to_number(reading->value, &number)) {
        bool integral = (double)(long long)number == number;

        /* An agent writes every value as text, so a position that happens to
         * arrive under the point map's default integer type must not lose its
         * fraction: only a value that really is whole becomes an integer. */
        if (dtype == NCL_DTYPE_FLOAT32 || dtype == NCL_DTYPE_FLOAT64 ||
            !integral) {
            return ncl_json_new_double(number);
        }
        return ncl_json_new_int((long long)number);
    }
    return ncl_json_new_string(reading->value);
}

static ncl_err mt_read_batch(ncl_driver *self, const ncl_address *addresses,
                             size_t count, ncl_json **values)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;
    char *xml = NULL;
    size_t xml_len = 0;
    char message[256];
    ncl_mt_current current;
    ncl_json *array;
    ncl_err result;
    size_t i;

    if (values == NULL || (count > 0 && addresses == NULL)) {
        return NCL_ERR_INVALID_ARG;
    }
    *values = NULL;
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (count == 0) {
        *values = array;
        return NCL_OK;
    }
    message[0] = '\0';
    result = mt_get(ctx, "/current", &xml, &xml_len, message, sizeof(message));
    if (result != NCL_OK) {
        ncl_json_free(array);
        return result;
    }
    ncl_mt_current_init(&current);
    result = ncl_mt_current_parse(xml, xml_len, &ctx->probe, &current, message,
                                  sizeof(message));
    ncl_free_safe(xml);
    if (result != NCL_OK) {
        ncl_mt_current_free(&current);
        ncl_json_free(array);
        return result;
    }
    ctx->last_sequence = current.last_sequence;
    for (i = 0; i < count; i++) {
        const ncl_mt_reading *reading =
            ncl_mt_current_find(&current, addresses[i].area);
        ncl_json *value;

        if (reading == NULL) {
            /* /current only reports what changed since it started (§6.1), so a
             * missing item is reported as having no value rather than as an
             * error: the caller decides whether that is stale or absent. */
            (void)ncl_json_arr_push(array, ncl_json_new_null());
            continue;
        }
        if (reading->category != NULL &&
            ncl_streq_ignore_case(reading->category, "CONDITION")) {
            mt_note_condition(ctx, reading);
        }
        value = mt_reading_to_json(reading, addresses[i].dtype);
        (void)ncl_json_arr_push(array, value != NULL ? value
                                                     : ncl_json_new_null());
    }
    *values = array;
    ncl_mt_current_free(&current);
    return NCL_OK;
}

/* ------------------------------------------------------------ the table -- */

static ncl_err mt_create(ncl_driver *self, const ncl_json *parameters)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;
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
    text = ncl_json_obj_get_string(parameters, "basePath");
    if (text != NULL) {
        ncl_free_safe(ctx->base_path);
        ctx->base_path = ncl_strdup(text);
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

/**
 * A session is the probe: it is what tells the driver which data items exist
 * and what category each one is. An agent that answers /current but not
 * /probe still works - the categories then come from the element names.
 */
static ncl_err mt_open(ncl_driver *self)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;
    char *xml = NULL;
    size_t xml_len = 0;
    char message[256];
    ncl_mt_probe probe;
    ncl_err result;

    ncl_mutex_lock(ctx->mutex);
    if (ctx->probe_absent) {
        /* Ask once: an agent that has no /probe does not grow one. */
        ncl_mutex_unlock(ctx->mutex);
        return NCL_OK;
    }
    message[0] = '\0';
    result = mt_get(ctx, "/probe", &xml, &xml_len, message, sizeof(message));
    if (result != NCL_OK) {
        ncl_mutex_unlock(ctx->mutex);
        if (ncl_driver_error_tier(result) == 1) {
            return result; /* the agent is not reachable at all */
        }
        /* It answered, but not with a probe document we can read: /current is
         * still worth reading, with the element names as the categories. */
        ctx->probe_absent = true;
        return NCL_OK;
    }
    ncl_mt_probe_init(&probe);
    result = ncl_mt_probe_parse(xml, xml_len, &probe, message, sizeof(message));
    ncl_free_safe(xml);
    if (result == NCL_OK) {
        ncl_mt_probe_free(&ctx->probe);
        ctx->probe = probe;
    }
    /* The device itself answered: keep the session even when its probe document
     * was not one we could read. */
    if (result != NCL_OK) {
        ctx->probe_absent = true;
    }
    ncl_mutex_unlock(ctx->mutex);
    return NCL_OK;
}

static void mt_close(ncl_driver *self)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;
    size_t i;

    ncl_mutex_lock(ctx->mutex);
    ncl_mt_probe_free(&ctx->probe);
    for (i = 0; i < ctx->state_count; i++) {
        ncl_free_safe(ctx->states[i].id);
        ncl_free_safe(ctx->states[i].state);
    }
    ctx->state_count = 0;
    ncl_mutex_unlock(ctx->mutex);
}

static bool mt_is_connected(const ncl_driver *self)
{
    const mt_ctx *ctx = (const mt_ctx *)self->ctx;

    /* HTTP has no session to hold: "connected" means the agent answered and we
     * still know its data items. */
    return ctx->probe.count > 0 || ctx->probe_absent;
}

static ncl_err mt_write_batch(ncl_driver *self, const ncl_address *addresses,
                              const ncl_json *values, size_t count)
{
    (void)self;
    (void)addresses;
    (void)values;
    (void)count;
    return NCL_ERR_NOT_SUPPORTED; /* MTConnect is a read-only interface (§1) */
}

/** Raw access: the frame is an HTTP path, the reply the document itself. */
static ncl_err mt_raw(ncl_driver *self, const void *frame, size_t frame_len,
                      ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    mt_ctx *ctx = (mt_ctx *)self->ctx;
    char path[512];
    char *body = NULL;
    size_t body_len = 0;
    char message[256];
    ncl_err result;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len == 0 || frame_len >= sizeof(path) ||
        out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* The caller gives a path ("/current"); a document name is accepted too. */
    if (((const char *)frame)[0] == '/') {
        memcpy(path, frame, frame_len);
        path[frame_len] = '\0';
    } else {
        snprintf(path, sizeof(path), "/%.*s", (int)frame_len,
                 (const char *)frame);
    }
    message[0] = '\0';
    {
        ncl_http_request request;

        memset(&request, 0, sizeof(request));
        request.host = ctx->host;
        request.port = ctx->port;
        request.path = path;
        request.user = ctx->user;
        request.password = ctx->password;
        request.timeout_ms = ctx->timeout_ms;
        result = ncl_http_get(&request, &body, &body_len, message,
                              sizeof(message));
    }
    if (result != NCL_OK) {
        return result;
    }
    ncl_driver_result_set_raw(out, body, body_len);
    hex = (char *)ncl_mem_alloc(body_len * 2u + 1u);
    if (hex == NULL) {
        ncl_free_safe(body);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < body_len; i++) {
        hex[i * 2] = kDigits[((unsigned char)body[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[(unsigned char)body[i] & 0xF];
    }
    hex[body_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    ncl_free_safe(body);
    return NCL_OK;
}

/** "probe" hands back the data item table; "sequence" the last seen number. */
static ncl_err mt_call(ncl_driver *self, const char *operation,
                       const ncl_json *params, ncl_json **result)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "probe")) {
        if (result != NULL) {
            *result = ncl_mt_probe_to_json(&ctx->probe);
            if (*result == NULL) {
                return NCL_ERR_NOMEM;
            }
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "sequence")) {
        if (result != NULL) {
            *result = ncl_json_new_object();
            if (*result == NULL) {
                return NCL_ERR_NOMEM;
            }
            (void)ncl_json_obj_set_int(*result, "sequence", ctx->last_sequence);
            (void)ncl_json_obj_set_int(*result, "dataItems",
                                       (long long)ctx->probe.count);
        }
        return NCL_OK;
    }
    return NCL_DRV_ERR_PROTOCOL(0x90); /* no such operation */
}

static void mt_attach_event(ncl_driver *self, ncl_driver_event_fn fn, void *user)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;

    /* The one event source this driver has: a condition going to Fault or
     * Warning, found while polling /current. */
    ctx->event_fn = fn;
    ctx->event_user = user;
}

static void mt_destroy(ncl_driver *self)
{
    mt_ctx *ctx;
    size_t i;

    if (self == NULL) {
        return;
    }
    ctx = (mt_ctx *)self->ctx;
    if (ctx != NULL) {
        ncl_mt_probe_free(&ctx->probe);
        for (i = 0; i < ctx->state_count; i++) {
            ncl_free_safe(ctx->states[i].id);
            ncl_free_safe(ctx->states[i].state);
        }
        ncl_free_safe(ctx->host);
        ncl_free_safe(ctx->base_path);
        ncl_free_safe(ctx->user);
        ncl_free_safe(ctx->password);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static ncl_err mt_read_batch_locked(ncl_driver *self,
                                    const ncl_address *addresses, size_t count,
                                    ncl_json **values)
{
    mt_ctx *ctx = (mt_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = mt_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static const ncl_driver_ops kMtOps = {
    "mtconnect",     mt_create,
    mt_open,         mt_close,
    mt_is_connected, mt_read_batch_locked,
    mt_write_batch,  mt_raw,
    mt_raw,          mt_call,
    mt_attach_event, mt_destroy,
};

ncl_driver *ncl_mtconnect_create(void)
{
    mt_ctx *ctx = (mt_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 7878;
    ctx->timeout_ms = 3000;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kMtOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
