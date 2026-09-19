/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the "mock" driver: an in-process memory model.
 *
 * A cell is one word (area + offset) holding either a number or a string; a
 * bit address is read and written through its word with a read-modify-write,
 * exactly like a real PLC. Unwritten cells read as zero, which is what a
 * device with blank memory does, so a point map can be exercised end to end
 * before anyone touches hardware.
 *
 * The failure injection ("fail") is what lets the adapter's tests walk the
 * three error tiers without a broken cable: ask for a transport, protocol or
 * business code and the next N operations return it.
 */

#include "mock/ncl_mock_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"

#define MOCK_TEXT_MAX 64
#define MOCK_HEX_MAX  512

typedef struct {
    char   *area; /**< owned; NULL means "no area" */
    int64_t offset;
    bool    is_text;
    double  num;
    char    text[MOCK_TEXT_MAX];
} mock_cell;

typedef struct {
    mock_cell *cells;
    size_t     count;
    size_t     cap;

    unsigned   latency_ms;
    bool       unmapped_is_error;

    int        fail_code;
    long       fail_remaining; /**< -1 = keep failing */

    bool       open;
    uint8_t   *raw; /**< last frame handed to write_raw */
    size_t     raw_len;

    ncl_driver_event_fn event_fn;
    void               *event_user;
} mock_ctx;

/* ------------------------------------------------------------- cells ----- */

static bool mock_area_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return ncl_str_is_empty(a) && ncl_str_is_empty(b);
    }
    return ncl_streq_ignore_case(a, b);
}

static mock_cell *mock_find(const mock_ctx *ctx, const char *area, int64_t offset)
{
    size_t i;

    for (i = 0; i < ctx->count; i++) {
        if (ctx->cells[i].offset == offset &&
            mock_area_equal(ctx->cells[i].area, area)) {
            return &ctx->cells[i];
        }
    }
    return NULL;
}

static mock_cell *mock_touch(mock_ctx *ctx, const char *area, int64_t offset)
{
    mock_cell *cell = mock_find(ctx, area, offset);

    if (cell != NULL) {
        return cell;
    }
    if (ctx->count == ctx->cap) {
        size_t cap = ctx->cap == 0 ? 16 : ctx->cap * 2;
        mock_cell *grown = (mock_cell *)ncl_mem_realloc(
            ctx->cells, cap * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }
        ctx->cells = grown;
        ctx->cap = cap;
    }
    cell = &ctx->cells[ctx->count++];
    memset(cell, 0, sizeof(*cell));
    if (!ncl_str_is_empty(area)) {
        cell->area = ncl_strdup(area);
        if (cell->area == NULL) {
            ctx->count--;
            return NULL;
        }
    }
    cell->offset = offset;
    return cell;
}

/** Integral value of a cell, truncated to the width of @p dtype. */
static int64_t mock_cell_integer(const mock_cell *cell, ncl_dtype dtype)
{
    int64_t value = (int64_t)cell->num;

    switch (dtype) {
    case NCL_DTYPE_BYTE:
        return value & 0xFF;
    case NCL_DTYPE_INT16:
        return (int64_t)(int16_t)(uint16_t)value;
    case NCL_DTYPE_INT32:
        return (int64_t)(int32_t)(uint32_t)value;
    default:
        return value;
    }
}

static ncl_json *mock_cell_to_json(const mock_cell *cell, ncl_dtype dtype)
{
    switch (dtype) {
    case NCL_DTYPE_STRING: {
        char text[MOCK_TEXT_MAX];

        if (cell->is_text) {
            return ncl_json_new_string(cell->text);
        }
        if (cell->num == (double)(int64_t)cell->num) {
            snprintf(text, sizeof(text), "%lld", (long long)cell->num);
        } else {
            snprintf(text, sizeof(text), "%g", cell->num);
        }
        return ncl_json_new_string(text);
    }
    case NCL_DTYPE_FLOAT32:
        return ncl_json_new_double((double)(float)cell->num);
    case NCL_DTYPE_FLOAT64:
        return ncl_json_new_double(cell->num);
    default:
        return ncl_json_new_int((long long)mock_cell_integer(cell, dtype));
    }
}

/** One element of an address: a bit, a text cell or a numeric word. */
static ncl_json *mock_read_element(mock_ctx *ctx, const ncl_address *addr,
                                   int64_t offset)
{
    mock_cell *cell = mock_find(ctx, addr->area, offset);

    if (cell == NULL) {
        if (!ctx->unmapped_is_error) {
            return addr->dtype == NCL_DTYPE_STRING
                       ? ncl_json_new_string("")
                       : ncl_json_new_int(0);
        }
        return NULL; /* the caller raises the business error */
    }
    if (addr->bit >= 0) {
        bool set = (mock_cell_integer(cell, NCL_DTYPE_INT16) >> addr->bit) & 1;

        return ncl_json_new_bool(set);
    }
    return mock_cell_to_json(cell, addr->dtype);
}

static ncl_err mock_write_element(mock_ctx *ctx, ncl_address *addr,
                                  int64_t offset, const ncl_json *value)
{
    mock_cell *cell = mock_touch(ctx, addr->area, offset);
    ncl_json_type type = ncl_json_type_of(value);

    if (cell == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (addr->bit >= 0) {
        bool set = false;
        int64_t word;

        if (!ncl_json_as_bool(value, &set)) {
            double number = 0;

            if (!ncl_json_as_double(value, &number)) {
                return NCL_ERR_INVALID_VALUE;
            }
            set = number != 0;
        }
        if (addr->bit > 15) {
            return NCL_ERR_INVALID_INDEX_RANGE;
        }
        word = mock_cell_integer(cell, NCL_DTYPE_INT16);
        word = set ? (word | (int64_t)1 << addr->bit)
                   : (word & ~((int64_t)1 << addr->bit));
        cell->is_text = false;
        cell->num = (double)(int16_t)(uint16_t)word;
        return NCL_OK;
    }
    if (addr->dtype == NCL_DTYPE_STRING || type == NCL_JSON_STRING) {
        const char *text = ncl_json_as_string(value);

        if (text == NULL) {
            return NCL_ERR_INVALID_VALUE;
        }
        cell->is_text = true;
        snprintf(cell->text, sizeof(cell->text), "%s", text);
        cell->num = 0;
        return NCL_OK;
    }
    {
        double number = 0;

        if (!ncl_json_as_double(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        cell->is_text = false;
        cell->num = number;
    }
    return NCL_OK;
}

/* ------------------------------------------------------------- helpers --- */

static int mock_inject(mock_ctx *ctx)
{
    if (ctx->fail_remaining == 0) {
        return 0;
    }
    if (ctx->fail_remaining > 0) {
        ctx->fail_remaining--;
    }
    return ctx->fail_code != 0 ? ctx->fail_code : NCL_DRV_ERR_TRANSPORT(1);
}

static void mock_wait(const mock_ctx *ctx)
{
    if (ctx->latency_ms > 0) {
        ncl_sleep_millis(ctx->latency_ms);
    }
}

static void hex_encode(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    static const char kHex[] = "0123456789abcdef";
    size_t used = (out_len - 1) / 2;
    size_t i;

    if (used > len) {
        used = len;
    }
    for (i = 0; i < used; i++) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[data[i] & 0xF];
    }
    out[used * 2] = '\0';
}

/* ---------------------------------------------------------------- ops ---- */

static ncl_err mock_create(ncl_driver *self, const ncl_json *parameters)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;
    const ncl_json *points;
    size_t i;

    if (parameters == NULL) {
        return NCL_OK;
    }
    ctx->latency_ms = (unsigned)ncl_json_obj_get_int(parameters, "latency_ms", 0);
    {
        const char *unmapped = ncl_json_obj_get_string(parameters, "unmapped");

        ctx->unmapped_is_error = unmapped != NULL &&
                                 ncl_streq_ignore_case(unmapped, "error");
    }
    {
        ncl_json *fail = ncl_json_obj_get(parameters, "fail");

        if (ncl_json_type_of(fail) == NCL_JSON_OBJECT) {
            ctx->fail_code = (int)ncl_json_obj_get_int(fail, "code", 0);
            ctx->fail_remaining = (long)ncl_json_obj_get_int(fail, "count", 1);
        }
    }

    points = ncl_json_obj_get(parameters, "points");
    for (i = 0; i < ncl_json_arr_len(points); i++) {
        ncl_address addr;
        ncl_json *value = ncl_json_obj_get(ncl_json_arr_get(points, i), "value");

        if (ncl_address_from_json(ncl_json_arr_get(points, i), &addr) != NCL_OK) {
            return NCL_ERR_INVALID_ARG;
        }
        if (value != NULL) {
            ncl_err err = mock_write_element(ctx, &addr, addr.offset, value);

            ncl_address_clear(&addr);
            if (err != NCL_OK) {
                return err;
            }
        } else {
            ncl_address_clear(&addr);
        }
    }
    return NCL_OK;
}

static ncl_err mock_open(ncl_driver *self)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;

    ctx->open = true;
    return NCL_OK;
}

static void mock_close(ncl_driver *self)
{
    ((mock_ctx *)self->ctx)->open = false;
}

static bool mock_is_connected(const ncl_driver *self)
{
    return ((const mock_ctx *)self->ctx)->open;
}

static ncl_err mock_read_batch(ncl_driver *self, const ncl_address *addresses,
                               size_t count, ncl_json **values)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;
    ncl_json *array;
    size_t i;
    int inject;

    if (values == NULL || (count > 0 && addresses == NULL)) {
        return NCL_ERR_INVALID_ARG;
    }
    *values = NULL;
    if (!ctx->open) {
        return NCL_DRV_ERR_TRANSPORT(1); /* 1 = session closed */
    }
    inject = mock_inject(ctx);
    if (inject != 0) {
        return inject;
    }
    mock_wait(ctx);

    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < count; i++) {
        const ncl_address *addr = &addresses[i];
        ncl_json *entry;
        int j;

        if (addr->length == 1) {
            entry = mock_read_element(ctx, addr, addr->offset);
            if (entry == NULL) {
                ncl_json_free(array);
                return NCL_DRV_ERR_BUSINESS(1); /* 1 = unmapped point */
            }
        } else {
            entry = ncl_json_new_array();
            if (entry == NULL) {
                ncl_json_free(array);
                return NCL_ERR_NOMEM;
            }
            for (j = 0; j < addr->length; j++) {
                ncl_json *element =
                    mock_read_element(ctx, addr, addr->offset + j);

                if (element == NULL) {
                    ncl_json_free(entry);
                    ncl_json_free(array);
                    return NCL_DRV_ERR_BUSINESS(1);
                }
                (void)ncl_json_arr_push(entry, element);
            }
        }
        (void)ncl_json_arr_push(array, entry);
    }
    *values = array;
    return NCL_OK;
}

static ncl_err mock_write_batch(ncl_driver *self, const ncl_address *addresses,
                                const ncl_json *values, size_t count)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;
    size_t i;
    int inject;

    if ((count > 0 && addresses == NULL) || values == NULL ||
        ncl_json_type_of(values) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(values) != count) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ctx->open) {
        return NCL_DRV_ERR_TRANSPORT(1);
    }
    inject = mock_inject(ctx);
    if (inject != 0) {
        return inject;
    }
    mock_wait(ctx);

    for (i = 0; i < count; i++) {
        ncl_address addr = addresses[i];
        ncl_json *value = ncl_json_arr_get(values, i);
        ncl_err err;

        if (addr.length == 1) {
            err = mock_write_element(ctx, &addr, addr.offset, value);
            if (err != NCL_OK) {
                return err;
            }
            continue;
        }
        if (ncl_json_type_of(value) != NCL_JSON_ARRAY ||
            ncl_json_arr_len(value) != (size_t)addr.length) {
            return NCL_ERR_INVALID_VALUE;
        }
        {
            int j;

            for (j = 0; j < addr.length; j++) {
                err = mock_write_element(ctx, &addr, addr.offset + j,
                                         ncl_json_arr_get(value, (size_t)j));
                if (err != NCL_OK) {
                    return err;
                }
            }
        }
    }
    return NCL_OK;
}

static ncl_err mock_write_raw(ncl_driver *self, const void *frame,
                              size_t frame_len, ncl_driver_result *out)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;
    char hex[MOCK_HEX_MAX];

    if (!ctx->open) {
        return NCL_DRV_ERR_TRANSPORT(1);
    }
    if (out == NULL || (frame_len > 0 && frame == NULL)) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_free_safe(ctx->raw);
    ctx->raw_len = 0;
    if (frame_len > 0) {
        ctx->raw = (uint8_t *)ncl_mem_alloc(frame_len);
        if (ctx->raw == NULL) {
            return NCL_ERR_NOMEM;
        }
        memcpy(ctx->raw, frame, frame_len);
        ctx->raw_len = frame_len;
    }
    /* A device answers a raw request with a raw reply: echo it. */
    ncl_driver_result_set_raw(out, ctx->raw, ctx->raw_len);
    hex_encode(ctx->raw, ctx->raw_len, hex, sizeof(hex));
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    return NCL_OK;
}

static ncl_err mock_read_raw(ncl_driver *self, const void *frame,
                             size_t frame_len, ncl_driver_result *out)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;
    char hex[MOCK_HEX_MAX];

    (void)frame;
    (void)frame_len;
    if (!ctx->open) {
        return NCL_DRV_ERR_TRANSPORT(1);
    }
    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ctx->raw == NULL || ctx->raw_len == 0) {
        return NCL_DRV_ERR_PROTOCOL(9); /* 9 = nothing has been written yet */
    }
    ncl_driver_result_set_raw(out, ctx->raw, ctx->raw_len);
    hex_encode(ctx->raw, ctx->raw_len, hex, sizeof(hex));
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    return NCL_OK;
}

static ncl_err mock_call(ncl_driver *self, const char *operation,
                         const ncl_json *params, ncl_json **result)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;

    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_str_is_blank(operation)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_streq_ignore_case(operation, "echo")) {
        if (result != NULL) {
            *result = ncl_json_clone(params);
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "sleep")) {
        long long ms = ncl_json_obj_get_int(params, "ms", 0);

        if (ms > 0) {
            ncl_sleep_millis((unsigned)ms);
        }
        if (result != NULL) {
            *result = ncl_json_new_object();
            if (*result != NULL) {
                (void)ncl_json_obj_set_int(*result, "slept", ms);
            }
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "raiseEvent")) {
        const char *id = ncl_json_obj_get_string(params, "id");
        ncl_json *event = ncl_json_obj_get(params, "event");
        bool delivered = false;

        if (ncl_str_is_empty(id)) {
            return NCL_ERR_INVALID_ARG;
        }
        if (ctx->event_fn != NULL) {
            ctx->event_fn(ctx->event_user, id, event);
            delivered = true;
        }
        if (result != NULL) {
            *result = ncl_json_new_object();
            if (*result != NULL) {
                (void)ncl_json_obj_set_bool(*result, "delivered", delivered);
            }
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "fail")) {
        return (int)ncl_json_obj_get_int(params, "code", NCL_DRV_ERR_BUSINESS(1));
    }
    return NCL_DRV_ERR_PROTOCOL(1); /* 1 = unknown operation */
}

static void mock_attach_event(ncl_driver *self, ncl_driver_event_fn fn, void *user)
{
    mock_ctx *ctx = (mock_ctx *)self->ctx;

    ctx->event_fn = fn;
    ctx->event_user = user;
}

static void mock_destroy(ncl_driver *self)
{
    mock_ctx *ctx;
    size_t i;

    if (self == NULL) {
        return;
    }
    ctx = (mock_ctx *)self->ctx;
    if (ctx != NULL) {
        for (i = 0; i < ctx->count; i++) {
            ncl_free_safe(ctx->cells[i].area);
        }
        ncl_free_safe(ctx->cells);
        ncl_free_safe(ctx->raw);
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kMockOps = {
    "mock",           mock_create,
    mock_open,        mock_close,
    mock_is_connected, mock_read_batch,
    mock_write_batch, mock_read_raw,
    mock_write_raw,   mock_call,
    mock_attach_event, mock_destroy,
};

ncl_driver *ncl_mock_driver_create(void)
{
    mock_ctx *ctx = (mock_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    driver = ncl_driver_new(&kMockOps, ctx);
    if (driver == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
