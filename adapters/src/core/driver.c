/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the protocol independent half of the driver interface.
 *
 * Everything in ncl_driver.h that does not depend on a wire format lives
 * here: the unified type names, the tiered error codes, the response
 * envelope, the on-demand session rule behind read_one()/write_one(), the
 * "D100.3" address parser and the protocol registry.
 *
 * A protocol driver only fills in ncl_driver_ops. Keeping the shared rules in
 * one place is what makes a new driver a single file of frame building and
 * parsing (see protocal/docs/00-通用-实现约定.md §1 and §2).
 */

#include "nclink_adapter/ncl_driver.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nclink/ncl_platform.h"

/* The drivers built into this library; more join in P1. */
#include "mock/ncl_mock_driver.h"
#include "fins/ncl_fins_driver.h"
#include "mc/ncl_mc_driver.h"
#include "modbus/ncl_modbus_driver.h"
#include "s7/ncl_s7_driver.h"

#define NCL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NCL_DRIVER_MAX_PROTOCOLS 64

/* ============================================================ data types == */

/* The seven unified types of 00-通用-实现约定 §1, in ncl_dtype order. */
static const char *const kDtypeNames[] = {
    "bit", "byte", "int16", "int32", "float32", "float64", "string",
};

/* Spellings a point map may use instead of the canonical name. */
static const struct {
    const char *text;
    ncl_dtype   dtype;
} kDtypeAliases[] = {
    {"bool", NCL_DTYPE_BIT},
    {"uint8", NCL_DTYPE_BYTE},
    {"u8", NCL_DTYPE_BYTE},
    {"i16", NCL_DTYPE_INT16},
    {"u16", NCL_DTYPE_INT16},
    {"uint16", NCL_DTYPE_INT16},
    {"short", NCL_DTYPE_INT16},
    {"i32", NCL_DTYPE_INT32},
    {"u32", NCL_DTYPE_INT32},
    {"uint32", NCL_DTYPE_INT32},
    {"int", NCL_DTYPE_INT32},
    {"float", NCL_DTYPE_FLOAT32},
    {"real", NCL_DTYPE_FLOAT32},
    {"double", NCL_DTYPE_FLOAT64},
    {"lreal", NCL_DTYPE_FLOAT64},
    {"text", NCL_DTYPE_STRING},
    {"str", NCL_DTYPE_STRING},
};

const char *ncl_dtype_name(ncl_dtype dtype)
{
    if ((int)dtype < 0 || (size_t)dtype >= NCL_ARRAY_LEN(kDtypeNames)) {
        return "?";
    }
    return kDtypeNames[dtype];
}

bool ncl_dtype_parse(const char *text, ncl_dtype *out)
{
    size_t i;

    if (ncl_str_is_blank(text)) {
        return false;
    }
    for (i = 0; i < NCL_ARRAY_LEN(kDtypeNames); i++) {
        if (ncl_streq_ignore_case(text, kDtypeNames[i])) {
            if (out != NULL) {
                *out = (ncl_dtype)i;
            }
            return true;
        }
    }
    for (i = 0; i < NCL_ARRAY_LEN(kDtypeAliases); i++) {
        if (ncl_streq_ignore_case(text, kDtypeAliases[i].text)) {
            if (out != NULL) {
                *out = kDtypeAliases[i].dtype;
            }
            return true;
        }
    }
    return false;
}

/* ================================================================ errors == */

int ncl_driver_error_tier(int code)
{
    if (code == 0) {
        return 0;
    }
    if (code < 0) {
        return -1; /* an ncl_err, not one of the three driver tiers */
    }
    return (code >> 28) & 0xF;
}

const char *ncl_driver_error_tier_name(int code)
{
    switch (ncl_driver_error_tier(code)) {
    case 0:  return "success";
    case 1:  return "transport";
    case 2:  return "protocol";
    case 3:  return "business";
    case -1: return "local";
    default: return "unknown";
    }
}

/* =============================================================== results == */

void ncl_driver_result_init(ncl_driver_result *result)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
}

void ncl_driver_result_free(ncl_driver_result *result)
{
    if (result == NULL) {
        return;
    }
    ncl_json_free(result->value);
    ncl_free_safe(result->raw);
    memset(result, 0, sizeof(*result));
}

void ncl_driver_result_ok(ncl_driver_result *result, ncl_json *value)
{
    if (result == NULL) {
        ncl_json_free(value);
        return;
    }
    /* Only the value is replaced: a driver may have already stashed the raw
     * reply with ncl_driver_result_set_raw(). */
    ncl_json_free(result->value);
    result->value = NULL;
    result->code = 0;
    result->success = true;
    result->value = value;
    result->message[0] = '\0';
}

void ncl_driver_result_fail(ncl_driver_result *result, int code,
                            const char *fmt, ...)
{
    va_list ap;

    if (result == NULL) {
        return;
    }
    ncl_json_free(result->value);
    result->value = NULL;
    result->code = code;
    result->success = false;
    if (fmt != NULL) {
        va_start(ap, fmt);
        vsnprintf(result->message, sizeof(result->message), fmt, ap);
        va_end(ap);
    }
}

void ncl_driver_result_set_raw(ncl_driver_result *result, const void *data,
                               size_t len)
{
    if (result == NULL) {
        return;
    }
    ncl_free_safe(result->raw);
    result->raw_len = 0;
    if (data == NULL || len == 0) {
        return;
    }
    result->raw = (uint8_t *)ncl_mem_alloc(len);
    if (result->raw == NULL) {
        return;
    }
    memcpy(result->raw, data, len);
    result->raw_len = len;
}

/* ============================================================= addresses == */

void ncl_address_clear(ncl_address *address)
{
    if (address == NULL) {
        return;
    }
    ncl_free_safe((void *)address->area);
    memset(address, 0, sizeof(*address));
    address->bit = -1;
}

char *ncl_address_to_text(const ncl_address *address)
{
    ncl_strbuf sb;
    char *text;

    if (address == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&sb);
    if (!ncl_str_is_empty(address->area)) {
        ncl_strbuf_puts(&sb, address->area);
    }
    ncl_strbuf_printf(&sb, "%lld", (long long)address->offset);
    if (address->bit >= 0) {
        ncl_strbuf_printf(&sb, ".%d", address->bit);
    }
    if (address->length != 1) {
        ncl_strbuf_printf(&sb, " x%d", address->length);
    }
    ncl_strbuf_printf(&sb, " %s", ncl_dtype_name(address->dtype));
    text = ncl_strbuf_detach(&sb);
    ncl_strbuf_free(&sb);
    return text;
}

/*
 * "D100", "M10.3", "DB1" - the area is the leading run of letters, then the
 * offset, then an optional ".bit". Modbus also writes its areas as "4x12"
 * (the traditional 4x/3x/1x/0x numbering), so a leading run of digits closed
 * by "x" is an area as well. Case is preserved; drivers compare areas
 * case-insensitively.
 */
static ncl_err address_parse_shorthand(const char *text, ncl_address *out)
{
    const char *p = text;
    long long offset = 0;

    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        p++;
    }
    if (p == text) {
        /* "4x12": digits closed by an 'x' name the area. */
        const char *digits = p;

        while (*p >= '0' && *p <= '9') {
            p++;
        }
        if (p != digits && (*p == 'x' || *p == 'X')) {
            p++;
        } else {
            p = text;
        }
    }
    if (p != text) {
        out->area = ncl_strndup(text, (size_t)(p - text));
        if (out->area == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    if (*p == '\0') {
        /* An area on its own ("CIO") means "at offset 0". */
        return p == text ? NCL_ERR_PARSE : NCL_OK;
    }
    if (*p < '0' || *p > '9') {
        return NCL_ERR_PARSE;
    }
    while (*p >= '0' && *p <= '9') {
        if (offset > (INT64_MAX - 9) / 10) {
            return NCL_ERR_RANGE;
        }
        offset = offset * 10 + (*p - '0');
        p++;
    }
    out->offset = offset;
    if (*p == '.') {
        int bit = 0;

        p++;
        if (*p < '0' || *p > '9') {
            return NCL_ERR_PARSE;
        }
        while (*p >= '0' && *p <= '9') {
            bit = bit * 10 + (*p - '0');
            if (bit > 63) {
                return NCL_ERR_RANGE;
            }
            p++;
        }
        out->bit = bit;
        out->dtype = NCL_DTYPE_BIT;
    }
    return *p == '\0' ? NCL_OK : NCL_ERR_PARSE;
}

ncl_err ncl_dtype_from_object(const ncl_json *object, const char *key,
                              ncl_dtype *out)
{
    const char *text;

    if (object == NULL || key == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_json_obj_has(object, key)) {
        return NCL_ERR_NOT_FOUND;
    }
    text = ncl_json_obj_get_string(object, key);
    if (text == NULL) {
        /* Tolerate {"dtype":16} as well as {"dtype":"int16"}. */
        ncl_json *value = ncl_json_obj_get(object, key);
        long long number = 0;

        if (ncl_json_as_int(value, &number) && number >= 0 &&
            (size_t)number < NCL_ARRAY_LEN(kDtypeNames)) {
            *out = (ncl_dtype)number;
            return NCL_OK;
        }
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    return ncl_dtype_parse(text, out) ? NCL_OK : NCL_ERR_INVALID_DATA_TYPE;
}

ncl_err ncl_address_from_json(const ncl_json *object, ncl_address *out)
{
    const char *area;
    ncl_err err = NCL_OK;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->bit = -1;
    out->length = 1;
    out->dtype = NCL_DTYPE_INT16;
    if (object == NULL) {
        return NCL_ERR_INVALID_ARG;
    }

    switch (ncl_json_type_of(object)) {
    case NCL_JSON_STRING:
        err = address_parse_shorthand(ncl_json_as_string(object), out);
        break;
    case NCL_JSON_OBJECT:
        area = ncl_json_obj_get_string(object, "area");
        if (area == NULL) {
            /* {"address":"D100"} is accepted as a synonym. */
            area = ncl_json_obj_get_string(object, "address");
        }
        if (ncl_str_is_blank(area)) {
            return NCL_ERR_INVALID_ARG;
        }
        if (ncl_json_obj_has(object, "offset")) {
            long long offset = 0;

            out->area = ncl_strdup(area);
            if (out->area == NULL) {
                return NCL_ERR_NOMEM;
            }
            if (!ncl_json_as_int(ncl_json_obj_get(object, "offset"), &offset)) {
                ncl_address_clear(out);
                return NCL_ERR_PARSE;
            }
            out->offset = offset;
            out->bit = (int)ncl_json_obj_get_int(object, "bit", -1);
        } else {
            err = address_parse_shorthand(area, out);
        }
        break;
    default:
        return NCL_ERR_PARSE;
    }
    if (err != NCL_OK) {
        ncl_address_clear(out);
        return err;
    }

    if (ncl_json_type_of(object) == NCL_JSON_OBJECT) {
        if (ncl_json_obj_has(object, "length")) {
            long long length = ncl_json_obj_get_int(object, "length", 1);

            if (length < 1 || length > 65536) {
                ncl_address_clear(out);
                return NCL_ERR_RANGE;
            }
            out->length = (int)length;
        }
        if (ncl_dtype_from_object(object, "dtype", &out->dtype) ==
            NCL_ERR_INVALID_DATA_TYPE) {
            ncl_address_clear(out);
            return NCL_ERR_INVALID_DATA_TYPE;
        }
    }
    if (out->bit >= 0) {
        out->dtype = NCL_DTYPE_BIT;
    } else if (out->dtype == NCL_DTYPE_BIT) {
        out->bit = 0; /* {"dtype":"bit"} without an index means bit 0 */
    }
    return NCL_OK;
}

/* ================================================================ driver == */

ncl_driver *ncl_driver_new(const ncl_driver_ops *ops, void *ctx)
{
    ncl_driver *driver;

    if (ops == NULL) {
        return NULL;
    }
    driver = (ncl_driver *)ncl_mem_calloc(1, sizeof(*driver));
    if (driver == NULL) {
        return NULL;
    }
    driver->ops = ops;
    driver->ctx = ctx;
    return driver;
}

const ncl_driver_ops *ncl_driver_ops_of(const ncl_driver *driver)
{
    return driver != NULL ? driver->ops : NULL;
}

const char *ncl_driver_protocol(const ncl_driver *driver)
{
    if (driver == NULL || driver->ops == NULL ||
        ncl_str_is_empty(driver->ops->protocol)) {
        return "?";
    }
    return driver->ops->protocol;
}

/*
 * Open the session on demand. A driver that can lose its session implements
 * both open() and is_connected(); one that has no session at all (the mock,
 * a purely computational driver) implements neither and is always ready.
 */
static ncl_err ensure_open(ncl_driver *driver)
{
    const ncl_driver_ops *ops = driver->ops;

    if (ops->open == NULL || ops->is_connected == NULL) {
        return NCL_OK;
    }
    if (ops->is_connected(driver)) {
        return NCL_OK;
    }
    return ops->open(driver);
}

/** True when @p addr can be handed to a driver. */
static bool address_is_usable(const ncl_address *addr)
{
    return addr != NULL && addr->length >= 1 && addr->offset >= 0 &&
           (int)addr->dtype >= 0 &&
           (size_t)addr->dtype < NCL_ARRAY_LEN(kDtypeNames);
}

ncl_err ncl_driver_read_one(ncl_driver *driver, const ncl_address *address,
                            ncl_json **value)
{
    ncl_json *values = NULL;
    ncl_err err;

    if (value != NULL) {
        *value = NULL;
    }
    if (driver == NULL || driver->ops == NULL || driver->ops->read_batch == NULL ||
        !address_is_usable(address) || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    err = ensure_open(driver);
    if (err != NCL_OK) {
        return err;
    }
    err = driver->ops->read_batch(driver, address, 1, &values);
    if (err != NCL_OK) {
        ncl_json_free(values);
        return err;
    }
    if (values == NULL || ncl_json_arr_len(values) != 1) {
        ncl_json_free(values);
        return NCL_ERR_STATE; /* a driver bug: wrong shape of reply */
    }
    *value = ncl_json_arr_take(values, 0);
    ncl_json_free(values);
    return NCL_OK;
}

ncl_err ncl_driver_write_one(ncl_driver *driver, const ncl_address *address,
                             const ncl_json *value)
{
    ncl_json *wrapped;
    ncl_err err;

    if (driver == NULL || driver->ops == NULL ||
        driver->ops->write_batch == NULL || !address_is_usable(address) ||
        value == NULL) {
        return NCL_ERR_NOT_SUPPORTED;
    }
    wrapped = ncl_json_new_array();
    if (wrapped == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (ncl_json_arr_push(wrapped, ncl_json_clone(value)) != NCL_OK) {
        ncl_json_free(wrapped);
        return NCL_ERR_NOMEM;
    }
    err = ensure_open(driver);
    if (err == NCL_OK) {
        err = driver->ops->write_batch(driver, address, wrapped, 1);
    }
    ncl_json_free(wrapped);
    return err;
}

/* ============================================================= factories == */

typedef struct {
    char              *name;
    ncl_driver_factory factory;
} protocol_slot;

static protocol_slot g_protocols[NCL_DRIVER_MAX_PROTOCOLS];
static size_t       g_protocol_count;
/* Lazily created, like the shared thread pool: registration is a startup
 * activity, so this only has to protect against a late concurrent caller. */
static ncl_mutex   *g_registry_mutex;
static bool         g_builtin_done;

static ncl_mutex *registry_lock(void)
{
    if (g_registry_mutex == NULL) {
        g_registry_mutex = ncl_mutex_create();
    }
    return g_registry_mutex;
}

static size_t registry_find(const char *protocol)
{
    size_t i;

    for (i = 0; i < g_protocol_count; i++) {
        if (ncl_streq_ignore_case(g_protocols[i].name, protocol)) {
            return i;
        }
    }
    return NCL_DRIVER_MAX_PROTOCOLS;
}

ncl_err ncl_driver_register_protocol(const char *protocol,
                                     ncl_driver_factory factory)
{
    ncl_mutex *lock;
    ncl_err err = NCL_OK;

    if (ncl_str_is_blank(protocol) || factory == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    lock = registry_lock();
    ncl_mutex_lock(lock);
    if (registry_find(protocol) != NCL_DRIVER_MAX_PROTOCOLS) {
        err = NCL_ERR_EXISTS;
    } else if (g_protocol_count >= NCL_DRIVER_MAX_PROTOCOLS) {
        err = NCL_ERR_RANGE;
    } else {
        char *name = ncl_strdup(protocol);

        if (name == NULL) {
            err = NCL_ERR_NOMEM;
        } else {
            g_protocols[g_protocol_count].name = name;
            g_protocols[g_protocol_count].factory = factory;
            g_protocol_count++;
        }
    }
    ncl_mutex_unlock(lock);
    return err;
}

void ncl_driver_register_builtin(void)
{
    ncl_mutex *lock = registry_lock();

    ncl_mutex_lock(lock);
    if (g_builtin_done) {
        ncl_mutex_unlock(lock);
        return;
    }
    g_builtin_done = true;
    ncl_mutex_unlock(lock);

    /* Idempotent: NCL_ERR_EXISTS means an embedder registered its own. */
    (void)ncl_driver_register_protocol("mock", ncl_mock_driver_create);
    (void)ncl_driver_register_protocol("modbus_tcp", ncl_modbus_tcp_create);
    (void)ncl_driver_register_protocol("modbus_rtu", ncl_modbus_rtu_create);
    (void)ncl_driver_register_protocol("modbus_rtu_tcp",
                                       ncl_modbus_rtu_tcp_create);
    (void)ncl_driver_register_protocol("mc_tcp", ncl_mc_tcp_create);
    (void)ncl_driver_register_protocol("fins_tcp", ncl_fins_tcp_create);
    (void)ncl_driver_register_protocol("s7_tcp", ncl_s7_tcp_create);
}

ncl_driver *ncl_driver_create(const char *protocol)
{
    ncl_driver_factory factory = NULL;
    ncl_mutex *lock;
    size_t index;

    if (ncl_str_is_blank(protocol)) {
        return NULL;
    }
    ncl_driver_register_builtin(); /* the built-ins are always available */
    lock = registry_lock();
    ncl_mutex_lock(lock);
    index = registry_find(protocol);
    if (index != NCL_DRIVER_MAX_PROTOCOLS) {
        factory = g_protocols[index].factory;
    }
    ncl_mutex_unlock(lock);
    /* Created outside the lock: a factory is free to touch the registry. */
    return factory != NULL ? factory() : NULL;
}

size_t ncl_driver_protocol_count(void)
{
    size_t count;
    ncl_mutex *lock = registry_lock();

    ncl_mutex_lock(lock);
    count = g_protocol_count;
    ncl_mutex_unlock(lock);
    return count;
}
