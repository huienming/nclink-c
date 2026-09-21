/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - error names, string helpers, string buffer, pointer vector. */
#include "nclink/ncl_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"

/* ------------------------------------------------------------ error names -- */

const char *ncl_err_name(ncl_err err)
{
    switch (err) {
    case NCL_OK: return "OK";
    case NCL_ERR: return "Error";
    case NCL_ERR_NOMEM: return "OutOfMemory";
    case NCL_ERR_PARSE: return "JsonParseException";
    case NCL_ERR_TIMEOUT: return "TimeoutException";
    case NCL_ERR_IO: return "IOException";
    case NCL_ERR_NOT_FOUND: return "NotFoundException";
    case NCL_ERR_EXISTS: return "AlreadyExistsException";
    case NCL_ERR_NOT_SUPPORTED: return "UnsupportedOperationException";
    case NCL_ERR_INVALID_ARG: return "IllegalArgumentException";
    case NCL_ERR_STATE: return "IllegalStateException";
    case NCL_ERR_RANGE: return "IndexOutOfBoundsException";
    case NCL_ERR_CONNECT: return "MqttException";
    case NCL_ERR_CLOSED: return "ClosedException";
    case NCL_ERR_NO_CHANNEL: return "NoFileChannelException";
    case NCL_ERR_UNAVAILABLE: return "UnavailableException";
    case NCL_ERR_INVALID_CODE: return "InvalidCodeException";
    case NCL_ERR_INVALID_DATA_NAME: return "InvalidDataNameException";
    case NCL_ERR_INVALID_DATA_TYPE: return "InvalidDataTypException";
    case NCL_ERR_INVALID_DEVICE_ID: return "InvalidDeviceIdException";
    case NCL_ERR_INVALID_ENCODING: return "InvalidEncodingException";
    case NCL_ERR_INVALID_ID: return "InvalidIdException";
    case NCL_ERR_INVALID_INDEX_RANGE: return "InvalidIndexRangeException";
    case NCL_ERR_INVALID_ITEM: return "InvalidItemException";
    case NCL_ERR_INVALID_KEY: return "InvalidKeyException";
    case NCL_ERR_INVALID_MESSAGE: return "InvalidMessageException";
    case NCL_ERR_INVALID_MESSAGE_ID: return "InvalidMessageIdException";
    case NCL_ERR_INVALID_MODEL: return "InvalidModelException";
    case NCL_ERR_INVALID_NODE: return "InvalidNodeException";
    case NCL_ERR_INVALID_NUMBER: return "InvalidNumberException";
    case NCL_ERR_INVALID_REQUEST: return "InvalidRequestException";
    case NCL_ERR_INVALID_TYPE: return "InvalidTypeException";
    case NCL_ERR_INVALID_VALUE: return "InvalidValueException";
    case NCL_ERR_INVALID_VERSION: return "InvalidVersionException";
    default: return "UnknownError";
    }
}

/* ----------------------------------------------------------- string utils -- */

char *ncl_strdup(const char *s)
{
    size_t len;
    char *copy;
    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    copy = (char *)ncl_mem_alloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

char *ncl_strndup(const char *s, size_t len)
{
    char *copy;
    if (s == NULL) {
        return NULL;
    }
    copy = (char *)ncl_mem_alloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

ncl_err ncl_vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list probe;
    int needed;

    if (out == NULL || fmt == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;

#if defined(NCL_OS_WINDOWS)
    va_copy(probe, ap);
    needed = _vscprintf(fmt, probe);
    va_end(probe);
#else
    va_copy(probe, ap);
    needed = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
#endif
    if (needed < 0) {
        return NCL_ERR;
    }
    *out = (char *)ncl_mem_alloc((size_t)needed + 1);
    if (*out == NULL) {
        return NCL_ERR_NOMEM;
    }
#if defined(NCL_OS_WINDOWS)
    vsnprintf(*out, (size_t)needed + 1, fmt, ap);
#else
    vsnprintf(*out, (size_t)needed + 1, fmt, ap);
#endif
    return NCL_OK;
}

ncl_err ncl_asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    ncl_err rc;
    va_start(ap, fmt);
    rc = ncl_vasprintf(out, fmt, ap);
    va_end(ap);
    return rc;
}

bool ncl_streq_ignore_case(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return ncl_strcasecmp(a, b) == 0;
}

bool ncl_str_starts_with(const char *s, const char *prefix)
{
    size_t n;
    if (s == NULL || prefix == NULL) {
        return false;
    }
    n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

bool ncl_str_ends_with(const char *s, const char *suffix)
{
    size_t ls, lf;
    if (s == NULL || suffix == NULL) {
        return false;
    }
    ls = strlen(s);
    lf = strlen(suffix);
    if (lf > ls) {
        return false;
    }
    return memcmp(s + (ls - lf), suffix, lf) == 0;
}

bool ncl_str_is_empty(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static bool ncl_isspace_ascii(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

bool ncl_str_is_blank(const char *s)
{
    if (s == NULL) {
        return true;
    }
    while (*s != '\0') {
        if (!ncl_isspace_ascii(*s)) {
            return false;
        }
        s++;
    }
    return true;
}

char *ncl_str_trim_dup(const char *s)
{
    const char *begin;
    const char *end;
    if (s == NULL) {
        return NULL;
    }
    begin = s;
    while (*begin != '\0' && ncl_isspace_ascii(*begin)) {
        begin++;
    }
    end = begin + strlen(begin);
    while (end > begin && ncl_isspace_ascii(end[-1])) {
        end--;
    }
    return ncl_strndup(begin, (size_t)(end - begin));
}

void ncl_free_safe(void *ptr)
{
    if (ptr != NULL) {
        ncl_mem_free(ptr);
    }
}

ncl_err ncl_uuid4(char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    size_t i;

    if (out == NULL || out_len < 37) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_random_bytes(raw, sizeof(raw))) {
        return NCL_ERR;
    }
    raw[6] = (unsigned char)((raw[6] & 0x0F) | 0x40); /* version 4 */
    raw[8] = (unsigned char)((raw[8] & 0x3F) | 0x80); /* variant 1 */

    {
        size_t pos = 0;
        for (i = 0; i < sizeof(raw); i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                out[pos++] = '-';
            }
            out[pos++] = hex[(raw[i] >> 4) & 0x0F];
            out[pos++] = hex[raw[i] & 0x0F];
        }
        out[pos] = '\0';
    }
    return NCL_OK;
}

/* -------------------------------------------------------------- ncl_strbuf -- */

void ncl_strbuf_init(ncl_strbuf *sb)
{
    if (sb != NULL) {
        sb->data = NULL;
        sb->len = 0;
        sb->cap = 0;
    }
}

void ncl_strbuf_free(ncl_strbuf *sb)
{
    if (sb == NULL) {
        return;
    }
    ncl_mem_free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void ncl_strbuf_reset(ncl_strbuf *sb)
{
    if (sb == NULL) {
        return;
    }
    sb->len = 0;
    if (sb->data != NULL) {
        sb->data[0] = '\0';
    }
}

ncl_err ncl_strbuf_reserve(ncl_strbuf *sb, size_t additional)
{
    size_t need;
    size_t cap;
    char *grown;

    if (sb == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    need = sb->len + additional + 1; /* +1 for NUL */
    if (need <= sb->cap && sb->data != NULL) {
        return NCL_OK;
    }
    cap = sb->cap == 0 ? 64 : sb->cap;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            return NCL_ERR_NOMEM;
        }
        cap *= 2;
    }
    grown = (char *)ncl_mem_realloc(sb->data, cap);
    if (grown == NULL) {
        return NCL_ERR_NOMEM;
    }
    sb->data = grown;
    sb->cap = cap;
    if (sb->len == 0) {
        sb->data[0] = '\0';
    }
    return NCL_OK;
}

ncl_err ncl_strbuf_append(ncl_strbuf *sb, const char *data, size_t len)
{
    ncl_err rc;
    if (sb == NULL || (data == NULL && len > 0)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return NCL_OK;
    }
    rc = ncl_strbuf_reserve(sb, len);
    if (rc != NCL_OK) {
        return rc;
    }
    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return NCL_OK;
}

ncl_err ncl_strbuf_puts(ncl_strbuf *sb, const char *s)
{
    if (s == NULL) {
        return NCL_OK;
    }
    return ncl_strbuf_append(sb, s, strlen(s));
}

ncl_err ncl_strbuf_putc(ncl_strbuf *sb, char c)
{
    ncl_err rc = ncl_strbuf_reserve(sb, 1);
    if (rc != NCL_OK) {
        return rc;
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
    return NCL_OK;
}

ncl_err ncl_strbuf_printf(ncl_strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    char stack_buf[256];
    char *heap_buf = NULL;
    int needed;
    ncl_err rc;

    if (sb == NULL || fmt == NULL) {
        return NCL_ERR_INVALID_ARG;
    }

    va_start(ap, fmt);
#if defined(NCL_OS_WINDOWS)
    {
        va_list probe;
        va_copy(probe, ap);
        needed = _vscprintf(fmt, probe);
        va_end(probe);
    }
#else
    {
        va_list probe;
        va_copy(probe, ap);
        needed = vsnprintf(NULL, 0, fmt, probe);
        va_end(probe);
    }
#endif
    if (needed < 0) {
        va_end(ap);
        return NCL_ERR;
    }

    if ((size_t)needed < sizeof(stack_buf)) {
        vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
        va_end(ap);
        return ncl_strbuf_append(sb, stack_buf, (size_t)needed);
    }

    heap_buf = (char *)ncl_mem_alloc((size_t)needed + 1);
    if (heap_buf == NULL) {
        va_end(ap);
        return NCL_ERR_NOMEM;
    }
    vsnprintf(heap_buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);

    rc = ncl_strbuf_append(sb, heap_buf, (size_t)needed);
    ncl_mem_free(heap_buf);
    return rc;
}

char *ncl_strbuf_detach(ncl_strbuf *sb)
{
    char *out;
    if (sb == NULL) {
        return NULL;
    }
    if (sb->data == NULL) {
        out = ncl_strdup("");
    } else {
        out = sb->data;
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

const char *ncl_strbuf_cstr(ncl_strbuf *sb)
{
    static const char empty[] = "";
    if (sb == NULL) {
        return empty;
    }
    if (sb->data == NULL) {
        if (ncl_strbuf_reserve(sb, 1) != NCL_OK) {
            return empty;
        }
        sb->data[0] = '\0';
    }
    return sb->data;
}

/* ----------------------------------------------------------------- ptrvec -- */

void ncl_ptrvec_init(ncl_ptrvec *v, ncl_free_fn free_fn)
{
    if (v != NULL) {
        v->items = NULL;
        v->len = 0;
        v->cap = 0;
        v->free_fn = free_fn;
    }
}

void ncl_ptrvec_clear(ncl_ptrvec *v)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    if (v->free_fn != NULL) {
        for (i = 0; i < v->len; i++) {
            if (v->items[i] != NULL) {
                v->free_fn(v->items[i]);
            }
        }
    }
    v->len = 0;
}

void ncl_ptrvec_free(ncl_ptrvec *v)
{
    if (v == NULL) {
        return;
    }
    ncl_ptrvec_clear(v);
    ncl_mem_free(v->items);
    v->items = NULL;
    v->cap = 0;
}

ncl_err ncl_ptrvec_push(ncl_ptrvec *v, void *item)
{
    if (v == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (v->len == v->cap) {
        size_t cap = v->cap == 0 ? 8 : v->cap * 2;
        void **grown = (void **)ncl_mem_realloc(v->items, cap * sizeof(void *));
        if (grown == NULL) {
            return NCL_ERR_NOMEM;
        }
        v->items = grown;
        v->cap = cap;
    }
    v->items[v->len++] = item;
    return NCL_OK;
}

ncl_err ncl_ptrvec_push_owned(ncl_ptrvec *v, void *item)
{
    ncl_err rc = ncl_ptrvec_push(v, item);
    if (rc != NCL_OK && v != NULL && v->free_fn != NULL && item != NULL) {
        v->free_fn(item);
    }
    return rc;
}

void *ncl_ptrvec_at(const ncl_ptrvec *v, size_t index)
{
    if (v == NULL || index >= v->len) {
        return NULL;
    }
    return v->items[index];
}

size_t ncl_ptrvec_len(const ncl_ptrvec *v)
{
    return v == NULL ? 0 : v->len;
}

void *ncl_ptrvec_take(ncl_ptrvec *v, size_t index)
{
    void *item;
    if (v == NULL || index >= v->len) {
        return NULL;
    }
    item = v->items[index];
    memmove(&v->items[index], &v->items[index + 1],
            (v->len - index - 1) * sizeof(void *));
    v->len--;
    return item;
}

/* ----------------------------------------------------------------- strvec -- */

void ncl_strvec_init(ncl_strvec *v)
{
    if (v != NULL) {
        v->items = NULL;
        v->len = 0;
        v->cap = 0;
    }
}

void ncl_strvec_clear(ncl_strvec *v)
{
    size_t i;
    if (v == NULL) {
        return;
    }
    for (i = 0; i < v->len; i++) {
        ncl_mem_free(v->items[i]);
    }
    v->len = 0;
}

void ncl_strvec_free(ncl_strvec *v)
{
    if (v == NULL) {
        return;
    }
    ncl_strvec_clear(v);
    ncl_mem_free(v->items);
    v->items = NULL;
    v->cap = 0;
}

ncl_err ncl_strvec_push(ncl_strvec *v, const char *s)
{
    char *copy;
    if (v == NULL || s == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (v->len == v->cap) {
        size_t cap = v->cap == 0 ? 8 : v->cap * 2;
        char **grown = (char **)ncl_mem_realloc(v->items, cap * sizeof(char *));
        if (grown == NULL) {
            return NCL_ERR_NOMEM;
        }
        v->items = grown;
        v->cap = cap;
    }
    copy = ncl_strdup(s);
    if (copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    v->items[v->len++] = copy;
    return NCL_OK;
}

const char *ncl_strvec_at(const ncl_strvec *v, size_t index)
{
    if (v == NULL || index >= v->len) {
        return NULL;
    }
    return v->items[index];
}

size_t ncl_strvec_len(const ncl_strvec *v)
{
    return v == NULL ? 0 : v->len;
}

bool ncl_strvec_contains(const ncl_strvec *v, const char *s)
{
    size_t i;
    if (v == NULL || s == NULL) {
        return false;
    }
    for (i = 0; i < v->len; i++) {
        if (strcmp(v->items[i], s) == 0) {
            return true;
        }
    }
    return false;
}
