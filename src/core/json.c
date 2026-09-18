/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - JSON DOM implementation. */
#include "nclink/ncl_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ncl_json {
    ncl_json_type type;
    union {
        bool boolean;
        struct {
            char  *raw;    /**< literal as parsed, or NULL for built values */
            double dbl;
            long long integer;
            bool  is_integer;
            bool  has_dbl;
            bool  has_int;
            bool  from_int; /**< built through ncl_json_new_int */
        } number;
        struct {
            char  *ptr;
            size_t len;
        } string;
        struct {
            ncl_json **items;
            size_t    len;
            size_t    cap;
        } array;
        struct {
            char     **keys;
            ncl_json **vals;
            size_t     len;
            size_t     cap;
        } object;
    } u;
};

/* ------------------------------------------------------------- allocation -- */

static ncl_json *ncl_json_alloc(ncl_json_type type)
{
    ncl_json *j = (ncl_json *)ncl_mem_calloc(1, sizeof(ncl_json));
    if (j != NULL) {
        j->type = type;
    }
    return j;
}

ncl_json *ncl_json_new_null(void)
{
    return ncl_json_alloc(NCL_JSON_NULL);
}

ncl_json *ncl_json_new_bool(bool value)
{
    ncl_json *j = ncl_json_alloc(NCL_JSON_BOOL);
    if (j != NULL) {
        j->u.boolean = value;
    }
    return j;
}

ncl_json *ncl_json_new_int(long long value)
{
    ncl_json *j = ncl_json_alloc(NCL_JSON_NUMBER);
    if (j != NULL) {
        j->u.number.integer = value;
        j->u.number.is_integer = true;
        j->u.number.has_int = true;
        j->u.number.from_int = true;
        j->u.number.dbl = (double)value;
        j->u.number.has_dbl = true;
    }
    return j;
}

ncl_json *ncl_json_new_double(double value)
{
    ncl_json *j = ncl_json_alloc(NCL_JSON_NUMBER);
    if (j != NULL) {
        j->u.number.dbl = value;
        j->u.number.has_dbl = true;
        if (value == (double)(long long)value && fabs(value) < 1e15) {
            j->u.number.integer = (long long)value;
            j->u.number.is_integer = true;
            j->u.number.has_int = true;
        }
    }
    return j;
}

ncl_json *ncl_json_new_string_len(const char *value, size_t len)
{
    ncl_json *j;
    if (value == NULL) {
        return ncl_json_new_null();
    }
    j = ncl_json_alloc(NCL_JSON_STRING);
    if (j == NULL) {
        return NULL;
    }
    j->u.string.ptr = (char *)ncl_mem_alloc(len + 1);
    if (j->u.string.ptr == NULL) {
        ncl_mem_free(j);
        return NULL;
    }
    if (len > 0) {
        memcpy(j->u.string.ptr, value, len);
    }
    j->u.string.ptr[len] = '\0';
    j->u.string.len = len;
    return j;
}

ncl_json *ncl_json_new_string(const char *value)
{
    if (value == NULL) {
        return ncl_json_new_null();
    }
    return ncl_json_new_string_len(value, strlen(value));
}

ncl_json *ncl_json_new_array(void)
{
    return ncl_json_alloc(NCL_JSON_ARRAY);
}

ncl_json *ncl_json_new_object(void)
{
    return ncl_json_alloc(NCL_JSON_OBJECT);
}

void ncl_json_free(ncl_json *j)
{
    size_t i;
    if (j == NULL) {
        return;
    }
    switch (j->type) {
    case NCL_JSON_STRING:
        ncl_mem_free(j->u.string.ptr);
        break;
    case NCL_JSON_NUMBER:
        ncl_mem_free(j->u.number.raw);
        break;
    case NCL_JSON_ARRAY:
        for (i = 0; i < j->u.array.len; i++) {
            ncl_json_free(j->u.array.items[i]);
        }
        ncl_mem_free(j->u.array.items);
        break;
    case NCL_JSON_OBJECT:
        for (i = 0; i < j->u.object.len; i++) {
            ncl_mem_free(j->u.object.keys[i]);
            ncl_json_free(j->u.object.vals[i]);
        }
        ncl_mem_free(j->u.object.keys);
        ncl_mem_free(j->u.object.vals);
        break;
    default:
        break;
    }
    ncl_mem_free(j);
}

ncl_json_type ncl_json_type_of(const ncl_json *j)
{
    return j == NULL ? NCL_JSON_NULL : j->type;
}

bool ncl_json_is_null(const ncl_json *j)
{
    return j == NULL || j->type == NCL_JSON_NULL;
}

bool ncl_json_is_number(const ncl_json *j)
{
    return j != NULL && j->type == NCL_JSON_NUMBER;
}

/* ----------------------------------------------------------------- object -- */

static size_t ncl_json_obj_index_of(const ncl_json *obj, const char *key)
{
    size_t i;
    if (obj == NULL || obj->type != NCL_JSON_OBJECT || key == NULL) {
        return (size_t)-1;
    }
    for (i = 0; i < obj->u.object.len; i++) {
        if (strcmp(obj->u.object.keys[i], key) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

ncl_json *ncl_json_obj_get(const ncl_json *obj, const char *key)
{
    size_t idx = ncl_json_obj_index_of(obj, key);
    if (idx == (size_t)-1) {
        return NULL;
    }
    return obj->u.object.vals[idx];
}

bool ncl_json_obj_has(const ncl_json *obj, const char *key)
{
    return ncl_json_obj_index_of(obj, key) != (size_t)-1;
}

size_t ncl_json_obj_len(const ncl_json *obj)
{
    if (obj == NULL || obj->type != NCL_JSON_OBJECT) {
        return 0;
    }
    return obj->u.object.len;
}

const char *ncl_json_obj_key_at(const ncl_json *obj, size_t index)
{
    if (obj == NULL || obj->type != NCL_JSON_OBJECT || index >= obj->u.object.len) {
        return NULL;
    }
    return obj->u.object.keys[index];
}

ncl_json *ncl_json_obj_val_at(const ncl_json *obj, size_t index)
{
    if (obj == NULL || obj->type != NCL_JSON_OBJECT || index >= obj->u.object.len) {
        return NULL;
    }
    return obj->u.object.vals[index];
}

ncl_err ncl_json_obj_remove(ncl_json *obj, const char *key)
{
    size_t idx;
    if (obj == NULL || obj->type != NCL_JSON_OBJECT) {
        return NCL_ERR_INVALID_ARG;
    }
    idx = ncl_json_obj_index_of(obj, key);
    if (idx == (size_t)-1) {
        return NCL_ERR_NOT_FOUND;
    }
    ncl_mem_free(obj->u.object.keys[idx]);
    ncl_json_free(obj->u.object.vals[idx]);
    memmove(&obj->u.object.keys[idx], &obj->u.object.keys[idx + 1],
            (obj->u.object.len - idx - 1) * sizeof(char *));
    memmove(&obj->u.object.vals[idx], &obj->u.object.vals[idx + 1],
            (obj->u.object.len - idx - 1) * sizeof(ncl_json *));
    obj->u.object.len--;
    return NCL_OK;
}

ncl_err ncl_json_obj_set(ncl_json *obj, const char *key, ncl_json *value)
{
    size_t idx;
    char *key_copy;

    if (obj == NULL || obj->type != NCL_JSON_OBJECT || key == NULL) {
        ncl_json_free(value);
        return NCL_ERR_INVALID_ARG;
    }
    if (value == NULL) {
        ncl_err rc = ncl_json_obj_remove(obj, key);
        return rc == NCL_ERR_NOT_FOUND ? NCL_OK : rc;
    }

    idx = ncl_json_obj_index_of(obj, key);
    if (idx != (size_t)-1) {
        ncl_json_free(obj->u.object.vals[idx]);
        obj->u.object.vals[idx] = value;
        return NCL_OK;
    }

    if (obj->u.object.len == obj->u.object.cap) {
        size_t cap = obj->u.object.cap == 0 ? 8 : obj->u.object.cap * 2;
        char **keys = (char **)ncl_mem_realloc(obj->u.object.keys, cap * sizeof(char *));
        ncl_json **vals;
        if (keys == NULL) {
            ncl_json_free(value);
            return NCL_ERR_NOMEM;
        }
        obj->u.object.keys = keys;
        vals = (ncl_json **)ncl_mem_realloc(obj->u.object.vals, cap * sizeof(ncl_json *));
        if (vals == NULL) {
            ncl_json_free(value);
            return NCL_ERR_NOMEM;
        }
        obj->u.object.vals = vals;
        obj->u.object.cap = cap;
    }

    key_copy = ncl_strdup(key);
    if (key_copy == NULL) {
        ncl_json_free(value);
        return NCL_ERR_NOMEM;
    }
    obj->u.object.keys[obj->u.object.len] = key_copy;
    obj->u.object.vals[obj->u.object.len] = value;
    obj->u.object.len++;
    return NCL_OK;
}

ncl_err ncl_json_obj_set_string(ncl_json *obj, const char *key, const char *value)
{
    ncl_json *j;
    if (value == NULL) {
        return ncl_json_obj_set(obj, key, NULL);
    }
    j = ncl_json_new_string(value);
    return ncl_json_obj_set(obj, key, j);
}

ncl_err ncl_json_obj_set_int(ncl_json *obj, const char *key, long long value)
{
    return ncl_json_obj_set(obj, key, ncl_json_new_int(value));
}

ncl_err ncl_json_obj_set_double(ncl_json *obj, const char *key, double value)
{
    return ncl_json_obj_set(obj, key, ncl_json_new_double(value));
}

ncl_err ncl_json_obj_set_bool(ncl_json *obj, const char *key, bool value)
{
    return ncl_json_obj_set(obj, key, ncl_json_new_bool(value));
}

ncl_err ncl_json_obj_set_null(ncl_json *obj, const char *key)
{
    return ncl_json_obj_set(obj, key, ncl_json_new_null());
}

const char *ncl_json_obj_get_string(const ncl_json *obj, const char *key)
{
    return ncl_json_as_string(ncl_json_obj_get(obj, key));
}

long long ncl_json_obj_get_int(const ncl_json *obj, const char *key, long long def)
{
    long long out = def;
    if (!ncl_json_as_int(ncl_json_obj_get(obj, key), &out)) {
        return def;
    }
    return out;
}

double ncl_json_obj_get_double(const ncl_json *obj, const char *key, double def)
{
    double out = def;
    if (!ncl_json_as_double(ncl_json_obj_get(obj, key), &out)) {
        return def;
    }
    return out;
}

bool ncl_json_obj_get_bool(const ncl_json *obj, const char *key, bool def)
{
    bool out = def;
    if (!ncl_json_as_bool(ncl_json_obj_get(obj, key), &out)) {
        return def;
    }
    return out;
}

/* ------------------------------------------------------------------ array -- */

ncl_err ncl_json_arr_push(ncl_json *arr, ncl_json *value)
{
    if (arr == NULL || arr->type != NCL_JSON_ARRAY) {
        ncl_json_free(value);
        return NCL_ERR_INVALID_ARG;
    }
    if (value == NULL) {
        value = ncl_json_new_null();
        if (value == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    if (arr->u.array.len == arr->u.array.cap) {
        size_t cap = arr->u.array.cap == 0 ? 8 : arr->u.array.cap * 2;
        ncl_json **grown =
            (ncl_json **)ncl_mem_realloc(arr->u.array.items, cap * sizeof(ncl_json *));
        if (grown == NULL) {
            ncl_json_free(value);
            return NCL_ERR_NOMEM;
        }
        arr->u.array.items = grown;
        arr->u.array.cap = cap;
    }
    arr->u.array.items[arr->u.array.len++] = value;
    return NCL_OK;
}

ncl_json *ncl_json_arr_get(const ncl_json *arr, size_t index)
{
    if (arr == NULL || arr->type != NCL_JSON_ARRAY || index >= arr->u.array.len) {
        return NULL;
    }
    return arr->u.array.items[index];
}

size_t ncl_json_arr_len(const ncl_json *arr)
{
    if (arr == NULL || arr->type != NCL_JSON_ARRAY) {
        return 0;
    }
    return arr->u.array.len;
}

ncl_json *ncl_json_arr_take(ncl_json *arr, size_t index)
{
    ncl_json *item;
    if (arr == NULL || arr->type != NCL_JSON_ARRAY || index >= arr->u.array.len) {
        return NULL;
    }
    item = arr->u.array.items[index];
    memmove(&arr->u.array.items[index], &arr->u.array.items[index + 1],
            (arr->u.array.len - index - 1) * sizeof(ncl_json *));
    arr->u.array.len--;
    return item;
}

/* ----------------------------------------------------------------- scalar -- */

const char *ncl_json_as_string(const ncl_json *j)
{
    if (j == NULL || j->type != NCL_JSON_STRING) {
        return NULL;
    }
    return j->u.string.ptr;
}

const char *ncl_json_number_raw(const ncl_json *j)
{
    if (j == NULL || j->type != NCL_JSON_NUMBER) {
        return NULL;
    }
    return j->u.number.raw;
}

static void ncl_json_number_materialise(ncl_json *j)
{
    if (j->u.number.raw != NULL) {
        const char *raw = j->u.number.raw;
        bool is_int_literal = true;
        const char *p = raw;
        if (*p == '-') {
            p++;
        }
        for (; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                is_int_literal = false;
                break;
            }
        }
        if (is_int_literal && *raw != '\0') {
            j->u.number.integer = strtoll(raw, NULL, 10);
            j->u.number.has_int = true;
            j->u.number.is_integer = true;
        }
        j->u.number.dbl = strtod(raw, NULL);
        j->u.number.has_dbl = true;
    }
}

bool ncl_json_as_int(const ncl_json *j, long long *out)
{
    if (j == NULL || out == NULL) {
        return false;
    }
    if (j->type == NCL_JSON_NUMBER) {
        /* Local copy so that lazy materialisation is possible. */
        ncl_json *mutable_j = (ncl_json *)j;
        if (!j->u.number.has_int) {
            ncl_json_number_materialise(mutable_j);
        }
        if (mutable_j->u.number.has_int) {
            *out = mutable_j->u.number.integer;
            return true;
        }
        {
            double d = mutable_j->u.number.dbl;
            double rounded = (d < 0) ? ceil(d - 0.5) : floor(d + 0.5);
            if (fabs(d - rounded) > 1e-9) {
                return false;
            }
            *out = (long long)rounded;
            return true;
        }
    }
    if (j->type == NCL_JSON_STRING) {
        const char *s = j->u.string.ptr;
        char *end = NULL;
        long long v;
        if (ncl_str_is_blank(s)) {
            return false;
        }
        v = strtoll(s, &end, 10);
        if (end == s || *end != '\0') {
            return false;
        }
        *out = v;
        return true;
    }
    if (j->type == NCL_JSON_BOOL) {
        *out = j->u.boolean ? 1 : 0;
        return true;
    }
    return false;
}

bool ncl_json_as_double(const ncl_json *j, double *out)
{
    if (j == NULL || out == NULL) {
        return false;
    }
    if (j->type == NCL_JSON_NUMBER) {
        ncl_json *mutable_j = (ncl_json *)j;
        if (!j->u.number.has_dbl) {
            ncl_json_number_materialise(mutable_j);
        }
        *out = mutable_j->u.number.dbl;
        return true;
    }
    if (j->type == NCL_JSON_STRING) {
        const char *s = j->u.string.ptr;
        char *end = NULL;
        double v;
        if (ncl_str_is_blank(s)) {
            return false;
        }
        v = strtod(s, &end);
        if (end == s || *end != '\0') {
            return false;
        }
        *out = v;
        return true;
    }
    if (j->type == NCL_JSON_BOOL) {
        *out = j->u.boolean ? 1.0 : 0.0;
        return true;
    }
    return false;
}

bool ncl_json_as_bool(const ncl_json *j, bool *out)
{
    if (j == NULL || out == NULL) {
        return false;
    }
    if (j->type == NCL_JSON_BOOL) {
        *out = j->u.boolean;
        return true;
    }
    if (j->type == NCL_JSON_STRING) {
        if (ncl_streq_ignore_case(j->u.string.ptr, "true")) {
            *out = true;
            return true;
        }
        if (ncl_streq_ignore_case(j->u.string.ptr, "false")) {
            *out = false;
            return true;
        }
        return false;
    }
    if (j->type == NCL_JSON_NUMBER) {
        long long v = 0;
        if (ncl_json_as_int(j, &v)) {
            *out = (v != 0);
            return true;
        }
        return false;
    }
    return false;
}

char *ncl_json_as_text(const ncl_json *j)
{
    if (j == NULL) {
        return NULL;
    }
    switch (j->type) {
    case NCL_JSON_STRING:
        return ncl_strdup(j->u.string.ptr);
    case NCL_JSON_NUMBER: {
        ncl_strbuf sb;
        const char *raw = j->u.number.raw;
        if (raw != NULL) {
            return ncl_strdup(raw);
        }
        ncl_strbuf_init(&sb);
        if (j->u.number.has_int && j->u.number.is_integer) {
            ncl_strbuf_printf(&sb, "%lld", j->u.number.integer);
        } else {
            ncl_strbuf_printf(&sb, "%.17g", j->u.number.dbl);
        }
        return ncl_strbuf_detach(&sb);
    }
    case NCL_JSON_BOOL:
        return ncl_strdup(j->u.boolean ? "true" : "false");
    default:
        return NULL;
    }
}

/* ----------------------------------------------------------------- writer -- */

static void ncl_json_write_escaped(const char *s, size_t len, ncl_strbuf *out)
{
    size_t i;
    ncl_strbuf_putc(out, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': ncl_strbuf_puts(out, "\\\""); break;
        case '\\': ncl_strbuf_puts(out, "\\\\"); break;
        case '\n': ncl_strbuf_puts(out, "\\n"); break;
        case '\r': ncl_strbuf_puts(out, "\\r"); break;
        case '\t': ncl_strbuf_puts(out, "\\t"); break;
        case '\b': ncl_strbuf_puts(out, "\\b"); break;
        case '\f': ncl_strbuf_puts(out, "\\f"); break;
        default:
            if (c < 0x20) {
                ncl_strbuf_printf(out, "\\u%04X", (unsigned)c);
            } else {
                ncl_strbuf_putc(out, (char)c);
            }
            break;
        }
    }
    ncl_strbuf_putc(out, '"');
}

static void ncl_json_write_number(const ncl_json *j, ncl_strbuf *out)
{
    if (j->u.number.raw != NULL) {
        ncl_strbuf_puts(out, j->u.number.raw);
        return;
    }
    if (j->u.number.from_int) {
        ncl_strbuf_printf(out, "%lld", j->u.number.integer);
        return;
    }
    if (j->u.number.has_dbl) {
        double d = j->u.number.dbl;
        if (fabs(d) < 1e15 && d == (double)(long long)d) {
            /* An integral double keeps its ".0" suffix. */
            ncl_strbuf_printf(out, "%lld.0", (long long)d);
        } else {
            /* Shortest representation that round trips, like Double.toString. */
            char buf[40];
            int precision;
            for (precision = 1; precision <= 17; precision++) {
                double back;
                snprintf(buf, sizeof(buf), "%.*g", precision, d);
                back = strtod(buf, NULL);
                if (back == d) {
                    break;
                }
            }
            if (precision > 17) {
                snprintf(buf, sizeof(buf), "%.17g", d);
            }
            ncl_strbuf_puts(out, buf);
        }
        return;
    }
    ncl_strbuf_puts(out, "0");
}

static ncl_err ncl_json_write_internal(const ncl_json *j, ncl_strbuf *out, int depth)
{
    size_t i;
    if (j == NULL) {
        return ncl_strbuf_puts(out, "null");
    }
    if (depth > NCL_JSON_MAX_DEPTH) {
        return NCL_ERR_RANGE;
    }
    switch (j->type) {
    case NCL_JSON_NULL:
        return ncl_strbuf_puts(out, "null");
    case NCL_JSON_BOOL:
        return ncl_strbuf_puts(out, j->u.boolean ? "true" : "false");
    case NCL_JSON_NUMBER:
        ncl_json_write_number(j, out);
        return NCL_OK;
    case NCL_JSON_STRING:
        ncl_json_write_escaped(j->u.string.ptr, j->u.string.len, out);
        return NCL_OK;
    case NCL_JSON_ARRAY:
        ncl_strbuf_putc(out, '[');
        for (i = 0; i < j->u.array.len; i++) {
            if (i > 0) {
                ncl_strbuf_putc(out, ',');
            }
            ncl_json_write_internal(j->u.array.items[i], out, depth + 1);
        }
        return ncl_strbuf_putc(out, ']');
    case NCL_JSON_OBJECT:
        ncl_strbuf_putc(out, '{');
        for (i = 0; i < j->u.object.len; i++) {
            if (i > 0) {
                ncl_strbuf_putc(out, ',');
            }
            ncl_json_write_escaped(j->u.object.keys[i],
                                   strlen(j->u.object.keys[i]), out);
            ncl_strbuf_putc(out, ':');
            ncl_json_write_internal(j->u.object.vals[i], out, depth + 1);
        }
        return ncl_strbuf_putc(out, '}');
    default:
        return NCL_ERR;
    }
}

ncl_err ncl_json_write(const ncl_json *j, ncl_strbuf *out)
{
    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_json_write_internal(j, out, 0);
}

char *ncl_json_write_string(const ncl_json *j)
{
    ncl_strbuf sb;
    ncl_strbuf_init(&sb);
    if (ncl_json_write(j, &sb) != NCL_OK) {
        ncl_strbuf_free(&sb);
        return NULL;
    }
    return ncl_strbuf_detach(&sb);
}

/* ----------------------------------------------------------------- parser -- */

typedef struct {
    const char *text;
    size_t      len;
    size_t      pos;
    ncl_strbuf *err;
} ncl_json_parser;

static void ncl_json_parser_error(ncl_json_parser *p, const char *msg)
{
    if (p->err != NULL) {
        ncl_strbuf_printf(p->err, "JSON parse error at offset %zu: %s",
                          p->pos, msg);
    }
}

static void ncl_json_skip_ws(ncl_json_parser *p)
{
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static bool ncl_json_peek(ncl_json_parser *p, char *out)
{
    if (p->pos >= p->len) {
        return false;
    }
    *out = p->text[p->pos];
    return true;
}

static bool ncl_json_match_literal(ncl_json_parser *p, const char *literal)
{
    size_t n = strlen(literal);
    if (p->pos + n > p->len) {
        return false;
    }
    if (memcmp(p->text + p->pos, literal, n) != 0) {
        return false;
    }
    p->pos += n;
    return true;
}

static ncl_err ncl_json_utf8_encode(uint32_t cp, ncl_strbuf *out)
{
    if (cp <= 0x7F) {
        return ncl_strbuf_putc(out, (char)cp);
    }
    if (cp <= 0x7FF) {
        ncl_strbuf_putc(out, (char)(0xC0 | (cp >> 6)));
        return ncl_strbuf_putc(out, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp <= 0xFFFF) {
        ncl_strbuf_putc(out, (char)(0xE0 | (cp >> 12)));
        ncl_strbuf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        return ncl_strbuf_putc(out, (char)(0x80 | (cp & 0x3F)));
    }
    ncl_strbuf_putc(out, (char)(0xF0 | (cp >> 18)));
    ncl_strbuf_putc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
    ncl_strbuf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
    return ncl_strbuf_putc(out, (char)(0x80 | (cp & 0x3F)));
}

static bool ncl_json_hex4(ncl_json_parser *p, uint32_t *out)
{
    uint32_t value = 0;
    int i;
    if (p->pos + 4 > p->len) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        char c = p->text[p->pos + (size_t)i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    p->pos += 4;
    *out = value;
    return true;
}

static char *ncl_json_parse_string_raw(ncl_json_parser *p, size_t *out_len)
{
    ncl_strbuf sb;
    ncl_strbuf_init(&sb);

    if (!ncl_json_peek(p, &(char){0})) {
        return NULL;
    }
    if (p->text[p->pos] != '"') {
        ncl_json_parser_error(p, "expected '\"'");
        return NULL;
    }
    p->pos++;

    while (p->pos < p->len) {
        char c = p->text[p->pos++];
        if (c == '"') {
            *out_len = sb.len;
            return ncl_strbuf_detach(&sb);
        }
        if (c == '\\') {
            char esc;
            if (!ncl_json_peek(p, &esc)) {
                ncl_json_parser_error(p, "unterminated escape sequence");
                break;
            }
            p->pos++;
            switch (esc) {
            case '"': ncl_strbuf_putc(&sb, '"'); break;
            case '\\': ncl_strbuf_putc(&sb, '\\'); break;
            case '/': ncl_strbuf_putc(&sb, '/'); break;
            case 'b': ncl_strbuf_putc(&sb, '\b'); break;
            case 'f': ncl_strbuf_putc(&sb, '\f'); break;
            case 'n': ncl_strbuf_putc(&sb, '\n'); break;
            case 'r': ncl_strbuf_putc(&sb, '\r'); break;
            case 't': ncl_strbuf_putc(&sb, '\t'); break;
            case 'u': {
                uint32_t cp;
                if (!ncl_json_hex4(p, &cp)) {
                    ncl_json_parser_error(p, "invalid \\u escape");
                    ncl_strbuf_free(&sb);
                    return NULL;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low;
                    size_t save = p->pos;
                    if (p->pos + 1 < p->len && p->text[p->pos] == '\\' &&
                        p->text[p->pos + 1] == 'u') {
                        p->pos += 2;
                        if (!ncl_json_hex4(p, &low)) {
                            ncl_json_parser_error(p, "invalid low surrogate");
                            ncl_strbuf_free(&sb);
                            return NULL;
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 +
                                 ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            ncl_json_utf8_encode(cp, &sb);
                            cp = low;
                        }
                    } else {
                        p->pos = save;
                    }
                }
                ncl_json_utf8_encode(cp, &sb);
                break;
            }
            default:
                ncl_json_parser_error(p, "unsupported escape sequence");
                ncl_strbuf_free(&sb);
                return NULL;
            }
            continue;
        }
        if ((unsigned char)c < 0x20) {
            ncl_json_parser_error(p, "unescaped control character in string");
            ncl_strbuf_free(&sb);
            return NULL;
        }
        ncl_strbuf_putc(&sb, c);
    }

    ncl_json_parser_error(p, "unterminated string");
    ncl_strbuf_free(&sb);
    return NULL;
}

static ncl_json *ncl_json_parse_value(ncl_json_parser *p, int depth);

static ncl_json *ncl_json_parse_number(ncl_json_parser *p)
{
    size_t start = p->pos;
    ncl_json *j;
    size_t scan = p->pos;

    if (scan < p->len && (p->text[scan] == '-' || p->text[scan] == '+')) {
        scan++;
    }
    while (scan < p->len) {
        char c = p->text[scan];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') {
            scan++;
        } else {
            break;
        }
    }
    if (scan == start) {
        ncl_json_parser_error(p, "invalid number");
        return NULL;
    }

    j = ncl_json_new_double(0.0);
    if (j == NULL) {
        return NULL;
    }
    ncl_mem_free(j->u.number.raw);
    j->u.number.raw = ncl_strndup(p->text + start, scan - start);
    if (j->u.number.raw == NULL) {
        ncl_json_free(j);
        return NULL;
    }
    j->u.number.has_dbl = false;
    j->u.number.has_int = false;
    j->u.number.is_integer = false;
    j->u.number.from_int = false;
    p->pos = scan;
    return j;
}

static ncl_json *ncl_json_parse_array(ncl_json_parser *p, int depth)
{
    ncl_json *arr = ncl_json_new_array();
    if (arr == NULL) {
        return NULL;
    }
    p->pos++; /* consume '[' */
    ncl_json_skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == ']') {
        p->pos++;
        return arr;
    }
    for (;;) {
        ncl_json *item = ncl_json_parse_value(p, depth + 1);
        if (item == NULL) {
            ncl_json_free(arr);
            return NULL;
        }
        if (ncl_json_arr_push(arr, item) != NCL_OK) {
            ncl_json_free(arr);
            return NULL;
        }
        ncl_json_skip_ws(p);
        if (p->pos < p->len && p->text[p->pos] == ',') {
            p->pos++;
            ncl_json_skip_ws(p);
            continue;
        }
        if (p->pos < p->len && p->text[p->pos] == ']') {
            p->pos++;
            return arr;
        }
        ncl_json_parser_error(p, "expected ',' or ']'");
        ncl_json_free(arr);
        return NULL;
    }
}

static ncl_json *ncl_json_parse_object(ncl_json_parser *p, int depth)
{
    ncl_json *obj = ncl_json_new_object();
    if (obj == NULL) {
        return NULL;
    }
    p->pos++; /* consume '{' */
    ncl_json_skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == '}') {
        p->pos++;
        return obj;
    }
    for (;;) {
        char *key;
        size_t key_len = 0;
        ncl_json *value;

        ncl_json_skip_ws(p);
        key = ncl_json_parse_string_raw(p, &key_len);
        if (key == NULL) {
            ncl_json_free(obj);
            return NULL;
        }
        ncl_json_skip_ws(p);
        if (p->pos >= p->len || p->text[p->pos] != ':') {
            ncl_json_parser_error(p, "expected ':'");
            ncl_mem_free(key);
            ncl_json_free(obj);
            return NULL;
        }
        p->pos++;
        value = ncl_json_parse_value(p, depth + 1);
        if (value == NULL) {
            ncl_mem_free(key);
            ncl_json_free(obj);
            return NULL;
        }
        if (ncl_json_obj_set(obj, key, value) != NCL_OK) {
            ncl_mem_free(key);
            ncl_json_free(obj);
            return NULL;
        }
        ncl_mem_free(key);

        ncl_json_skip_ws(p);
        if (p->pos < p->len && p->text[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->text[p->pos] == '}') {
            p->pos++;
            return obj;
        }
        ncl_json_parser_error(p, "expected ',' or '}'");
        ncl_json_free(obj);
        return NULL;
    }
}

static ncl_json *ncl_json_parse_value(ncl_json_parser *p, int depth)
{
    char c = '\0';

    if (depth > NCL_JSON_MAX_DEPTH) {
        ncl_json_parser_error(p, "maximum nesting depth exceeded");
        return NULL;
    }
    ncl_json_skip_ws(p);
    if (!ncl_json_peek(p, &c)) {
        ncl_json_parser_error(p, "unexpected end of input");
        return NULL;
    }

    switch (c) {
    case '{':
        return ncl_json_parse_object(p, depth);
    case '[':
        return ncl_json_parse_array(p, depth);
    case '"': {
        size_t len = 0;
        char *s = ncl_json_parse_string_raw(p, &len);
        ncl_json *j;
        if (s == NULL) {
            return NULL;
        }
        j = ncl_json_new_string_len(s, len);
        ncl_mem_free(s);
        return j;
    }
    case 't':
        if (ncl_json_match_literal(p, "true")) {
            return ncl_json_new_bool(true);
        }
        ncl_json_parser_error(p, "invalid literal");
        return NULL;
    case 'f':
        if (ncl_json_match_literal(p, "false")) {
            return ncl_json_new_bool(false);
        }
        ncl_json_parser_error(p, "invalid literal");
        return NULL;
    case 'n':
        if (ncl_json_match_literal(p, "null")) {
            return ncl_json_new_null();
        }
        ncl_json_parser_error(p, "invalid literal");
        return NULL;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ncl_json_parse_number(p);
        }
        ncl_json_parser_error(p, "unexpected character");
        return NULL;
    }
}

ncl_json *ncl_json_parse(const char *text, size_t len, ncl_strbuf *err)
{
    ncl_json_parser parser;
    ncl_json *root;

    if (text == NULL) {
        if (err != NULL) {
            ncl_strbuf_puts(err, "JSON parse error: input is NULL");
        }
        return NULL;
    }

    parser.text = text;
    parser.len = len;
    parser.pos = 0;
    parser.err = err;

    root = ncl_json_parse_value(&parser, 0);
    if (root == NULL) {
        return NULL;
    }
    ncl_json_skip_ws(&parser);
    if (parser.pos != parser.len) {
        ncl_json_parser_error(&parser, "trailing content after document");
        ncl_json_free(root);
        return NULL;
    }
    return root;
}

ncl_json *ncl_json_parse_cstr(const char *text, ncl_strbuf *err)
{
    if (text == NULL) {
        return ncl_json_parse(NULL, 0, err);
    }
    return ncl_json_parse(text, strlen(text), err);
}

/* ------------------------------------------------------------ clone/equal -- */

ncl_json *ncl_json_clone(const ncl_json *j)
{
    size_t i;
    ncl_json *copy;
    if (j == NULL) {
        return NULL;
    }
    switch (j->type) {
    case NCL_JSON_NULL:
        return ncl_json_new_null();
    case NCL_JSON_BOOL:
        return ncl_json_new_bool(j->u.boolean);
    case NCL_JSON_NUMBER: {
        ncl_json *n = ncl_json_new_double(0.0);
        if (n == NULL) {
            return NULL;
        }
        ncl_mem_free(n->u.number.raw);
        n->u.number.raw = j->u.number.raw != NULL
                              ? ncl_strdup(j->u.number.raw)
                              : NULL;
        n->u.number.dbl = j->u.number.dbl;
        n->u.number.integer = j->u.number.integer;
        n->u.number.is_integer = j->u.number.is_integer;
        n->u.number.has_dbl = j->u.number.has_dbl;
        n->u.number.has_int = j->u.number.has_int;
        n->u.number.from_int = j->u.number.from_int;
        if (j->u.number.raw != NULL && n->u.number.raw == NULL) {
            ncl_json_free(n);
            return NULL;
        }
        return n;
    }
    case NCL_JSON_STRING:
        return ncl_json_new_string_len(j->u.string.ptr, j->u.string.len);
    case NCL_JSON_ARRAY:
        copy = ncl_json_new_array();
        if (copy == NULL) {
            return NULL;
        }
        for (i = 0; i < j->u.array.len; i++) {
            ncl_json *child = ncl_json_clone(j->u.array.items[i]);
            if (child == NULL || ncl_json_arr_push(copy, child) != NCL_OK) {
                ncl_json_free(copy);
                return NULL;
            }
        }
        return copy;
    case NCL_JSON_OBJECT:
        copy = ncl_json_new_object();
        if (copy == NULL) {
            return NULL;
        }
        for (i = 0; i < j->u.object.len; i++) {
            ncl_json *child = ncl_json_clone(j->u.object.vals[i]);
            if (child == NULL ||
                ncl_json_obj_set(copy, j->u.object.keys[i], child) != NCL_OK) {
                ncl_json_free(copy);
                return NULL;
            }
        }
        return copy;
    default:
        return NULL;
    }
}

static bool ncl_json_number_equals(const ncl_json *a, const ncl_json *b)
{
    const char *ra = a->u.number.raw != NULL ? a->u.number.raw : NULL;
    const char *rb = b->u.number.raw != NULL ? b->u.number.raw : NULL;
    double da = 0.0;
    double db = 0.0;

    if (ra != NULL && rb != NULL) {
        return strcmp(ra, rb) == 0;
    }

    if (a->u.number.is_integer && b->u.number.is_integer &&
        (a->u.number.has_int || ra != NULL) &&
        (b->u.number.has_int || rb != NULL)) {
        long long ia = 0;
        long long ib = 0;
        if (!ncl_json_as_int(a, &ia) || !ncl_json_as_int(b, &ib)) {
            return false;
        }
        return ia == ib;
    }

    if (!ncl_json_as_double(a, &da) || !ncl_json_as_double(b, &db)) {
        return false;
    }
    return da == db;
}

bool ncl_json_equals(const ncl_json *a, const ncl_json *b)
{
    size_t i;
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return ncl_json_is_null(a) && ncl_json_is_null(b);
    }
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
    case NCL_JSON_NULL:
        return true;
    case NCL_JSON_BOOL:
        return a->u.boolean == b->u.boolean;
    case NCL_JSON_NUMBER:
        return ncl_json_number_equals(a, b);
    case NCL_JSON_STRING:
        return a->u.string.len == b->u.string.len &&
               memcmp(a->u.string.ptr, b->u.string.ptr, a->u.string.len) == 0;
    case NCL_JSON_ARRAY:
        if (a->u.array.len != b->u.array.len) {
            return false;
        }
        for (i = 0; i < a->u.array.len; i++) {
            if (!ncl_json_equals(a->u.array.items[i], b->u.array.items[i])) {
                return false;
            }
        }
        return true;
    case NCL_JSON_OBJECT:
        if (a->u.object.len != b->u.object.len) {
            return false;
        }
        for (i = 0; i < a->u.object.len; i++) {
            ncl_json *other = ncl_json_obj_get(b, a->u.object.keys[i]);
            if (other == NULL) {
                return false;
            }
            if (!ncl_json_equals(a->u.object.vals[i], other)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ misc -- */

ncl_json *ncl_strvec_to_json(const ncl_strvec *v)
{
    ncl_json *arr = ncl_json_new_array();
    size_t i;
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_strvec_len(v); i++) {
        if (ncl_json_arr_push(arr, ncl_json_new_string(ncl_strvec_at(v, i))) !=
            NCL_OK) {
            ncl_json_free(arr);
            return NULL;
        }
    }
    return arr;
}
