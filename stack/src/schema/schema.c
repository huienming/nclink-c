/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - JSON Schema validator (draft-07 subset).
 *
 * The schema document is validated directly rather than compiled to a node
 * tree: the documents NC-Link validates are small and this keeps $ref
 * resolution trivial. `pattern` values are compiled once at load time and
 * cached, because validation may run on several pool threads.
 */
#include "nclink/ncl_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"

#if defined(NCL_OS_WINDOWS)
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#endif

typedef struct {
    char      *pattern;
    ncl_regex *regex;
} ncl_pattern_entry;

struct ncl_schema {
    ncl_json   *root;     /**< owned */
    ncl_ptrvec  patterns; /**< ncl_pattern_entry* */
    ncl_mutex  *mutex;
};

static void ncl_pattern_entry_free(void *element)
{
    ncl_pattern_entry *entry = (ncl_pattern_entry *)element;
    if (entry == NULL) {
        return;
    }
    ncl_mem_free(entry->pattern);
    ncl_regex_free(entry->regex);
    ncl_mem_free(entry);
}

/* --------------------------------------------------------------- helpers -- */

/** Type name used in the validation messages. */
static const char *ncl_json_type_label(const ncl_json *value)
{
    const char *raw;

    switch (ncl_json_type_of(value)) {
    case NCL_JSON_NULL:
        return "Null";
    case NCL_JSON_BOOL:
        return "Boolean";
    case NCL_JSON_STRING:
        return "String";
    case NCL_JSON_ARRAY:
        return "Array";
    case NCL_JSON_OBJECT:
        return "Object";
    case NCL_JSON_NUMBER:
        raw = ncl_json_number_raw(value);
        if (raw != NULL && strpbrk(raw, ".eE") == NULL) {
            return "Integer";
        }
        return "Number";
    default:
        return "Unknown";
    }
}

static void ncl_schema_error(ncl_strvec *errors, const char *path,
                             const char *fmt, ...)
{
    ncl_strbuf sb;
    va_list ap;
    char *body = NULL;

    if (errors == NULL) {
        return;
    }
    va_start(ap, fmt);
    ncl_vasprintf(&body, fmt, ap);
    va_end(ap);
    ncl_strbuf_init(&sb);
    ncl_strbuf_puts(&sb, path != NULL ? path : "#");
    ncl_strbuf_puts(&sb, ": ");
    ncl_strbuf_puts(&sb, body != NULL ? body : "");
    ncl_mem_free(body);
    ncl_strvec_push(errors, ncl_strbuf_cstr(&sb));
    ncl_strbuf_free(&sb);
}

/** Short rendering of a scalar for messages. */
static char *ncl_json_render(const ncl_json *value)
{
    if (value == NULL) {
        return ncl_strdup("null");
    }
    if (ncl_json_type_of(value) == NCL_JSON_STRING) {
        const char *text = ncl_json_as_string(value);
        ncl_strbuf sb;
        char *out;
        ncl_strbuf_init(&sb);
        ncl_strbuf_putc(&sb, '"');
        ncl_strbuf_puts(&sb, text != NULL ? text : "");
        ncl_strbuf_putc(&sb, '"');
        out = ncl_strbuf_detach(&sb);
        return out;
    }
    return ncl_json_write_string(value);
}

/** Escape one token for a JSON pointer ("~" -> "~0", "/" -> "~1"). */
static void ncl_pointer_escape(ncl_strbuf *sb, const char *token)
{
    for (; token != NULL && *token != '\0'; token++) {
        if (*token == '~') {
            ncl_strbuf_puts(sb, "~0");
        } else if (*token == '/') {
            ncl_strbuf_puts(sb, "~1");
        } else {
            ncl_strbuf_putc(sb, *token);
        }
    }
}

/** Append "/<token>" to @p path, escaping pointer specials. */
static char *ncl_pointer_push(const char *path, const char *token)
{
    ncl_strbuf sb;
    char *out;

    ncl_strbuf_init(&sb);
    ncl_strbuf_puts(&sb, path != NULL ? path : "#");
    ncl_strbuf_putc(&sb, '/');
    ncl_pointer_escape(&sb, token);
    out = ncl_strbuf_detach(&sb);
    return out;
}

static char *ncl_pointer_push_index(const char *path, size_t index)
{
    char token[32];
    snprintf(token, sizeof(token), "%u", (unsigned)index);
    return ncl_pointer_push(path, token);
}

/** Split a "#/a/b" pointer into tokens and walk @p root. */
static const ncl_json *ncl_pointer_resolve(const ncl_json *root, const char *ref)
{
    const ncl_json *current = root;
    const char *cursor;

    if (root == NULL || ref == NULL || ref[0] != '#') {
        return NULL;
    }
    cursor = ref + 1;
    while (*cursor != '\0') {
        char token[NCL_PATH_MAX_BUF];
        size_t used = 0;

        if (*cursor != '/') {
            return NULL;
        }
        cursor++;
        while (*cursor != '\0' && *cursor != '/' && used + 1 < sizeof(token)) {
            if (cursor[0] == '~' && cursor[1] == '0') {
                token[used++] = '~';
                cursor += 2;
                continue;
            }
            if (cursor[0] == '~' && cursor[1] == '1') {
                token[used++] = '/';
                cursor += 2;
                continue;
            }
            token[used++] = *cursor++;
        }
        token[used] = '\0';
        if (ncl_json_type_of(current) == NCL_JSON_OBJECT) {
            current = ncl_json_obj_get(current, token);
        } else if (ncl_json_type_of(current) == NCL_JSON_ARRAY) {
            long index = strtol(token, NULL, 10);
            current = index >= 0 ? ncl_json_arr_get(current, (size_t)index)
                                 : NULL;
        } else {
            return NULL;
        }
        if (current == NULL) {
            return NULL;
        }
    }
    return current;
}

/* ------------------------------------------------------------ type check -- */

static bool ncl_schema_type_matches(const ncl_json *value, const char *type)
{
    if (type == NULL || value == NULL) {
        return false;
    }
    if (strcmp(type, "null") == 0) {
        return ncl_json_type_of(value) == NCL_JSON_NULL;
    }
    if (strcmp(type, "boolean") == 0) {
        return ncl_json_type_of(value) == NCL_JSON_BOOL;
    }
    if (strcmp(type, "object") == 0) {
        return ncl_json_type_of(value) == NCL_JSON_OBJECT;
    }
    if (strcmp(type, "array") == 0) {
        return ncl_json_type_of(value) == NCL_JSON_ARRAY;
    }
    if (strcmp(type, "string") == 0) {
        return ncl_json_type_of(value) == NCL_JSON_STRING;
    }
    if (strcmp(type, "number") == 0) {
        return ncl_json_type_of(value) == NCL_JSON_NUMBER;
    }
    if (strcmp(type, "integer") == 0) {
        const char *raw;
        if (ncl_json_type_of(value) != NCL_JSON_NUMBER) {
            return false;
        }
        raw = ncl_json_number_raw(value);
        return raw != NULL && strpbrk(raw, ".eE") == NULL;
    }
    return false;
}

/** True when the schema's "type" keyword accepts @p value. */
static bool ncl_schema_accepts_type(const ncl_json *type_keyword,
                                    const ncl_json *value)
{
    if (type_keyword == NULL) {
        return true;
    }
    if (ncl_json_type_of(type_keyword) == NCL_JSON_STRING) {
        return ncl_schema_type_matches(value, ncl_json_as_string(type_keyword));
    }
    if (ncl_json_type_of(type_keyword) == NCL_JSON_ARRAY) {
        size_t i;
        for (i = 0; i < ncl_json_arr_len(type_keyword); i++) {
            const char *name =
                ncl_json_as_string(ncl_json_arr_get(type_keyword, i));
            if (ncl_schema_type_matches(value, name)) {
                return true;
            }
        }
        return false;
    }
    return true;
}

/* -------------------------------------------------------------- patterns -- */

static ncl_regex *ncl_schema_pattern(ncl_schema *schema, const char *pattern)
{
    size_t i;
    ncl_regex *regex = NULL;

    if (schema == NULL || pattern == NULL) {
        return NULL;
    }
    ncl_mutex_lock(schema->mutex);
    for (i = 0; i < schema->patterns.len; i++) {
        ncl_pattern_entry *entry =
            (ncl_pattern_entry *)schema->patterns.items[i];
        if (entry != NULL && strcmp(entry->pattern, pattern) == 0) {
            regex = entry->regex;
            break;
        }
    }
    if (regex == NULL) {
        ncl_pattern_entry *entry =
            (ncl_pattern_entry *)ncl_mem_calloc(1, sizeof(*entry));
        if (entry != NULL) {
            entry->pattern = ncl_strdup(pattern);
            entry->regex = ncl_regex_compile(pattern, NULL);
            if (entry->pattern != NULL &&
                ncl_ptrvec_push(&schema->patterns, entry) == NCL_OK) {
                regex = entry->regex;
            } else {
                ncl_pattern_entry_free(entry);
            }
        }
    }
    ncl_mutex_unlock(schema->mutex);
    return regex;
}

/* ------------------------------------------------------------- formats ---- */

static bool ncl_format_date_time(const char *text)
{
    /* RFC 3339: YYYY-MM-DDTHH:MM:SS[.fff][Z|±HH:MM] */
    size_t len = strlen(text);
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;

    if (len < 19) {
        return false;
    }
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour,
               &minute, &second) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60) {
        return false;
    }
    return text[10] == 'T' || text[10] == 't';
}

static bool ncl_format_date(const char *text)
{
    int year;
    int month;
    int day;

    if (strlen(text) != 10) {
        return false;
    }
    if (sscanf(text, "%4d-%2d-%2d", &year, &month, &day) != 3) {
        return false;
    }
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

static bool ncl_format_time(const char *text)
{
    int hour;
    int minute;
    int second;

    if (strlen(text) < 8) {
        return false;
    }
    if (sscanf(text, "%2d:%2d:%2d", &hour, &minute, &second) != 3) {
        return false;
    }
    return hour <= 23 && minute <= 59 && second <= 60;
}

static bool ncl_format_email(const char *text)
{
    const char *at = strchr(text, '@');
    const char *dot;

    if (at == NULL || at == text) {
        return false;
    }
    if (strchr(at + 1, '@') != NULL) {
        return false;
    }
    dot = strchr(at + 1, '.');
    return dot != NULL && dot != at + 1 && dot[1] != '\0';
}

static bool ncl_format_ipv4(const char *text)
{
    struct in_addr address;
    return inet_pton(AF_INET, text, &address) == 1;
}

static bool ncl_format_ipv6(const char *text)
{
    struct in6_addr address;
    return inet_pton(AF_INET6, text, &address) == 1;
}

static bool ncl_format_uuid(const char *text)
{
    size_t i;
    size_t len = strlen(text);

    if (len != 36) {
        return false;
    }
    for (i = 0; i < len; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return false;
            }
            continue;
        }
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool ncl_format_uri(const char *text)
{
    const char *colon = strchr(text, ':');

    if (colon == NULL || colon == text) {
        return false;
    }
    return (colon[1] == '/' && colon[2] == '/') || colon[1] != '\0';
}

/** Validate one `format` keyword. Unknown formats are always accepted. */
static bool ncl_format_ok(const char *format, const char *text)
{
    if (format == NULL || text == NULL) {
        return true;
    }
    if (strcmp(format, "date-time") == 0) {
        return ncl_format_date_time(text);
    }
    if (strcmp(format, "date") == 0) {
        return ncl_format_date(text);
    }
    if (strcmp(format, "time") == 0) {
        return ncl_format_time(text);
    }
    if (strcmp(format, "email") == 0) {
        return ncl_format_email(text);
    }
    if (strcmp(format, "ipv4") == 0) {
        return ncl_format_ipv4(text);
    }
    if (strcmp(format, "ipv6") == 0) {
        return ncl_format_ipv6(text);
    }
    if (strcmp(format, "uuid") == 0) {
        return ncl_format_uuid(text);
    }
    if (strcmp(format, "uri") == 0 || strcmp(format, "uri-reference") == 0) {
        return ncl_format_uri(text);
    }
    return true;
}

/* ----------------------------------------------------------- validation --- */

typedef struct {
    ncl_schema *schema;
    ncl_strvec *errors;
    int         ref_depth;
} ncl_validate_ctx;

static void ncl_validate_value(ncl_validate_ctx *ctx, const ncl_json *rule,
                               const ncl_json *value, const char *path);

static bool ncl_json_double_of(const ncl_json *value, double *out)
{
    return ncl_json_type_of(value) == NCL_JSON_NUMBER &&
           ncl_json_as_double(value, out);
}

static bool ncl_json_same(const ncl_json *a, const ncl_json *b)
{
    return ncl_json_equals(a, b);
}

static void ncl_validate_number(ncl_validate_ctx *ctx, const ncl_json *rule,
                                const ncl_json *value, const char *path)
{
    double actual;
    double limit;

    if (!ncl_json_double_of(value, &actual)) {
        return;
    }
    {
        ncl_json *keyword = ncl_json_obj_get(rule, "minimum");
        if (keyword != NULL && ncl_json_double_of(keyword, &limit) &&
            actual < limit) {
            ncl_schema_error(ctx->errors, path, "expected minimum: %g, found %g",
                             limit, actual);
        }
    }
    {
        ncl_json *keyword = ncl_json_obj_get(rule, "maximum");
        if (keyword != NULL && ncl_json_double_of(keyword, &limit) &&
            actual > limit) {
            ncl_schema_error(ctx->errors, path, "expected maximum: %g, found %g",
                             limit, actual);
        }
    }
    {
        ncl_json *keyword = ncl_json_obj_get(rule, "exclusiveMinimum");
        if (keyword != NULL) {
            if (ncl_json_type_of(keyword) == NCL_JSON_BOOL) {
                /* draft-04 style: boolean flag modifying `minimum`. */
                ncl_json *base = ncl_json_obj_get(rule, "minimum");
                if (ncl_json_obj_get_bool(rule, "exclusiveMinimum", false) &&
                    base != NULL && ncl_json_double_of(base, &limit) &&
                    actual <= limit) {
                    ncl_schema_error(ctx->errors, path,
                                     "expected exclusiveMinimum: %g, found %g",
                                     limit, actual);
                }
            } else if (ncl_json_double_of(keyword, &limit) &&
                       actual <= limit) {
                ncl_schema_error(ctx->errors, path,
                                 "expected exclusiveMinimum: %g, found %g",
                                 limit, actual);
            }
        }
    }
    {
        ncl_json *keyword = ncl_json_obj_get(rule, "exclusiveMaximum");
        if (keyword != NULL) {
            if (ncl_json_type_of(keyword) == NCL_JSON_BOOL) {
                ncl_json *base = ncl_json_obj_get(rule, "maximum");
                if (ncl_json_obj_get_bool(rule, "exclusiveMaximum", false) &&
                    base != NULL && ncl_json_double_of(base, &limit) &&
                    actual >= limit) {
                    ncl_schema_error(ctx->errors, path,
                                     "expected exclusiveMaximum: %g, found %g",
                                     limit, actual);
                }
            } else if (ncl_json_double_of(keyword, &limit) &&
                       actual >= limit) {
                ncl_schema_error(ctx->errors, path,
                                 "expected exclusiveMaximum: %g, found %g",
                                 limit, actual);
            }
        }
    }
    {
        ncl_json *keyword = ncl_json_obj_get(rule, "multipleOf");
        if (keyword != NULL && ncl_json_double_of(keyword, &limit) &&
            limit > 0) {
            double quotient = actual / limit;
            double rounded = (double)(long long)(quotient + (quotient < 0 ? -0.5 : 0.5));
            if (rounded * limit - actual > 1e-9 ||
                actual - rounded * limit > 1e-9) {
                ncl_schema_error(ctx->errors, path, "%g is not a multiple of %g",
                                 actual, limit);
            }
        }
    }
}

static void ncl_validate_string(ncl_validate_ctx *ctx, const ncl_json *rule,
                                const ncl_json *value, const char *path)
{
    const char *text = ncl_json_as_string(value);
    size_t length;
    ncl_json *keyword;

    if (text == NULL) {
        return;
    }
    length = strlen(text);
    keyword = ncl_json_obj_get(rule, "minLength");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_NUMBER &&
        (double)length < ncl_json_obj_get_double(rule, "minLength", 0)) {
        ncl_schema_error(ctx->errors, path, "expected minLength: %g, actual: %u",
                         ncl_json_obj_get_double(rule, "minLength", 0),
                         (unsigned)length);
    }
    keyword = ncl_json_obj_get(rule, "maxLength");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_NUMBER &&
        (double)length > ncl_json_obj_get_double(rule, "maxLength", 0)) {
        ncl_schema_error(ctx->errors, path, "expected maxLength: %g, actual: %u",
                         ncl_json_obj_get_double(rule, "maxLength", 0),
                         (unsigned)length);
    }
    keyword = ncl_json_obj_get(rule, "pattern");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_STRING) {
        const char *pattern = ncl_json_as_string(keyword);
        ncl_regex *regex = ncl_schema_pattern(ctx->schema, pattern);
        if (regex != NULL && !ncl_regex_search(regex, text, length)) {
            ncl_schema_error(ctx->errors, path,
                             "string [%s] does not match pattern %s", text,
                             pattern);
        }
    }
    keyword = ncl_json_obj_get(rule, "format");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_STRING) {
        const char *format = ncl_json_as_string(keyword);
        if (!ncl_format_ok(format, text)) {
            ncl_schema_error(ctx->errors, path,
                             "string [%s] is not a valid %s", text,
                             format != NULL ? format : "value");
        }
    }
}

static void ncl_validate_array(ncl_validate_ctx *ctx, const ncl_json *rule,
                               const ncl_json *value, const char *path)
{
    size_t length = ncl_json_arr_len(value);
    ncl_json *items = ncl_json_obj_get(rule, "items");
    ncl_json *keyword = ncl_json_obj_get(rule, "minItems");
    size_t i;

    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_NUMBER &&
        (double)length < ncl_json_obj_get_double(rule, "minItems", 0)) {
        ncl_schema_error(ctx->errors, path,
                         "expected minimum item count: %g, found: %u",
                         ncl_json_obj_get_double(rule, "minItems", 0),
                         (unsigned)length);
    }
    keyword = ncl_json_obj_get(rule, "maxItems");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_NUMBER &&
        (double)length > ncl_json_obj_get_double(rule, "maxItems", 0)) {
        ncl_schema_error(ctx->errors, path,
                         "expected maximum item count: %g, found: %u",
                         ncl_json_obj_get_double(rule, "maxItems", 0),
                         (unsigned)length);
    }
    if (ncl_json_obj_get_bool(rule, "uniqueItems", false)) {
        size_t a;
        for (a = 0; a < length; a++) {
            size_t b;
            for (b = a + 1; b < length; b++) {
                if (ncl_json_same(ncl_json_arr_get(value, a),
                                  ncl_json_arr_get(value, b))) {
                    ncl_schema_error(ctx->errors, path,
                                     "array items are not unique");
                    a = length;
                    break;
                }
            }
        }
    }
    if (items == NULL) {
        return;
    }
    if (ncl_json_type_of(items) == NCL_JSON_ARRAY) {
        size_t tuple_len = ncl_json_arr_len(items);
        ncl_json *extra = ncl_json_obj_get(rule, "additionalItems");
        for (i = 0; i < length; i++) {
            char *child = ncl_pointer_push_index(path, i);
            if (i < tuple_len) {
                ncl_validate_value(ctx, ncl_json_arr_get(items, i),
                                   ncl_json_arr_get(value, i), child);
            } else if (extra != NULL &&
                       ncl_json_type_of(extra) == NCL_JSON_OBJECT) {
                ncl_validate_value(ctx, extra, ncl_json_arr_get(value, i),
                                   child);
            } else if (extra != NULL && ncl_json_is_null(extra) == false &&
                       ncl_json_type_of(extra) == NCL_JSON_BOOL &&
                       !ncl_json_as_bool(extra, NULL)) {
                ncl_schema_error(ctx->errors, path,
                                 "array index %u is not permitted",
                                 (unsigned)i);
            }
            ncl_mem_free(child);
        }
        return;
    }
    for (i = 0; i < length; i++) {
        char *child = ncl_pointer_push_index(path, i);
        ncl_validate_value(ctx, items, ncl_json_arr_get(value, i), child);
        ncl_mem_free(child);
    }
}

static void ncl_validate_object(ncl_validate_ctx *ctx, const ncl_json *rule,
                                const ncl_json *value, const char *path)
{
    ncl_json *properties = ncl_json_obj_get(rule, "properties");
    ncl_json *required = ncl_json_obj_get(rule, "required");
    ncl_json *additional = ncl_json_obj_get(rule, "additionalProperties");
    size_t count = ncl_json_obj_len(value);
    size_t i;

    if (required != NULL && ncl_json_type_of(required) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(required); i++) {
            const char *name = ncl_json_as_string(ncl_json_arr_get(required, i));
            if (name != NULL && !ncl_json_obj_has(value, name)) {
                ncl_schema_error(ctx->errors, path,
                                 "required key [%s] not found", name);
            }
        }
    }
    {
        ncl_json *keyword = ncl_json_obj_get(rule, "minProperties");
        if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_NUMBER &&
            (double)count < ncl_json_obj_get_double(rule, "minProperties", 0)) {
            ncl_schema_error(ctx->errors, path,
                             "expected minimum property count: %g, found: %u",
                             ncl_json_obj_get_double(rule, "minProperties", 0),
                             (unsigned)count);
        }
        keyword = ncl_json_obj_get(rule, "maxProperties");
        if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_NUMBER &&
            (double)count > ncl_json_obj_get_double(rule, "maxProperties", 0)) {
            ncl_schema_error(ctx->errors, path,
                             "expected maximum property count: %g, found: %u",
                             ncl_json_obj_get_double(rule, "maxProperties", 0),
                             (unsigned)count);
        }
    }
    for (i = 0; i < count; i++) {
        const char *name = ncl_json_obj_key_at(value, i);
        ncl_json *member = ncl_json_obj_val_at(value, i);
        ncl_json *subrule =
            properties != NULL ? ncl_json_obj_get(properties, name) : NULL;
        char *child = ncl_pointer_push(path, name);

        if (subrule != NULL) {
            ncl_validate_value(ctx, subrule, member, child);
        } else if (additional != NULL &&
                   ncl_json_type_of(additional) == NCL_JSON_OBJECT) {
            ncl_validate_value(ctx, additional, member, child);
        } else if (additional != NULL && ncl_json_type_of(additional) == NCL_JSON_BOOL) {
            bool allowed = true;
            ncl_json_as_bool(additional, &allowed);
            if (!allowed) {
                ncl_schema_error(ctx->errors, path,
                                 "extraneous key [%s] is not permitted", name);
            }
        }
        ncl_mem_free(child);
    }
}

/**
 * Run @p rule against @p value but only collect the errors, so a combinator
 * (anyOf/oneOf/not) can decide whether the branch matched.
 */
static bool ncl_validate_silently(ncl_validate_ctx *ctx, const ncl_json *rule,
                                  const ncl_json *value, const char *path)
{
    ncl_strvec scratch;
    ncl_strvec *saved = ctx->errors;
    bool valid;

    ncl_strvec_init(&scratch);
    ctx->errors = &scratch;
    ncl_validate_value(ctx, rule, value, path);
    valid = scratch.len == 0;
    ncl_strvec_free(&scratch);
    ctx->errors = saved;
    return valid;
}

static void ncl_validate_combinators(ncl_validate_ctx *ctx,
                                     const ncl_json *rule,
                                     const ncl_json *value, const char *path)
{
    ncl_json *keyword = ncl_json_obj_get(rule, "allOf");
    size_t i;

    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(keyword); i++) {
            ncl_validate_value(ctx, ncl_json_arr_get(keyword, i), value, path);
        }
    }
    keyword = ncl_json_obj_get(rule, "anyOf");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_ARRAY) {
        bool matched = false;
        for (i = 0; i < ncl_json_arr_len(keyword) && !matched; i++) {
            matched = ncl_validate_silently(ctx, ncl_json_arr_get(keyword, i),
                                            value, path);
        }
        if (!matched) {
            ncl_schema_error(ctx->errors, path,
                             "anyOf: none of the %u subschemas matched",
                             (unsigned)ncl_json_arr_len(keyword));
        }
    }
    keyword = ncl_json_obj_get(rule, "oneOf");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_ARRAY) {
        unsigned matched = 0;
        for (i = 0; i < ncl_json_arr_len(keyword); i++) {
            if (ncl_validate_silently(ctx, ncl_json_arr_get(keyword, i), value,
                                      path)) {
                matched++;
            }
        }
        if (matched != 1) {
            ncl_schema_error(ctx->errors, path,
                             "oneOf: %u subschemas matched, expected exactly one",
                             matched);
        }
    }
    keyword = ncl_json_obj_get(rule, "not");
    if (keyword != NULL && ncl_validate_silently(ctx, keyword, value, path)) {
        ncl_schema_error(ctx->errors, path, "the `not` schema matched");
    }
    keyword = ncl_json_obj_get(rule, "if");
    if (keyword != NULL) {
        ncl_json *then_schema = ncl_json_obj_get(rule, "then");
        ncl_json *else_schema = ncl_json_obj_get(rule, "else");
        if (ncl_validate_silently(ctx, keyword, value, path)) {
            if (then_schema != NULL) {
                ncl_validate_value(ctx, then_schema, value, path);
            }
        } else if (else_schema != NULL) {
            ncl_validate_value(ctx, else_schema, value, path);
        }
    }
}

static void ncl_validate_enum(ncl_validate_ctx *ctx, const ncl_json *rule,
                              const ncl_json *value, const char *path)
{
    ncl_json *keyword = ncl_json_obj_get(rule, "enum");
    size_t i;

    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_ARRAY) {
        bool found = false;
        for (i = 0; i < ncl_json_arr_len(keyword); i++) {
            if (ncl_json_same(ncl_json_arr_get(keyword, i), value)) {
                found = true;
                break;
            }
        }
        if (!found) {
            char *rendered = ncl_json_render(value);
            ncl_schema_error(ctx->errors, path,
                             "value %s is not defined in the enum",
                             rendered != NULL ? rendered : "?");
            ncl_mem_free(rendered);
        }
    }
    keyword = ncl_json_obj_get(rule, "const");
    if (keyword != NULL && !ncl_json_same(keyword, value)) {
        char *expected = ncl_json_render(keyword);
        char *actual = ncl_json_render(value);
        ncl_schema_error(ctx->errors, path, "expected: %s, found: %s",
                         expected != NULL ? expected : "?",
                         actual != NULL ? actual : "?");
        ncl_mem_free(expected);
        ncl_mem_free(actual);
    }
}

static void ncl_validate_value(ncl_validate_ctx *ctx, const ncl_json *rule,
                               const ncl_json *value, const char *path)
{
    ncl_json *keyword;

    if (rule == NULL) {
        return;
    }
    if (ncl_json_type_of(rule) == NCL_JSON_BOOL) {
        bool allowed = true;
        ncl_json_as_bool(rule, &allowed);
        if (!allowed) {
            ncl_schema_error(ctx->errors, path, "the `false` schema rejects every value");
        }
        return;
    }
    if (ncl_json_type_of(rule) != NCL_JSON_OBJECT) {
        return;
    }

    keyword = ncl_json_obj_get(rule, "$ref");
    if (keyword != NULL && ncl_json_type_of(keyword) == NCL_JSON_STRING) {
        const ncl_json *target;

        if (ctx->ref_depth >= NCL_SCHEMA_MAX_REF_DEPTH) {
            ncl_schema_error(ctx->errors, path, "$ref nesting is too deep");
            return;
        }
        target = ncl_pointer_resolve(ctx->schema->root,
                                     ncl_json_as_string(keyword));
        if (target == NULL) {
            ncl_schema_error(ctx->errors, path, "cannot resolve $ref %s",
                             ncl_json_as_string(keyword));
            return;
        }
        ctx->ref_depth++;
        ncl_validate_value(ctx, target, value, path);
        ctx->ref_depth--;
        return; /* draft-07: siblings of $ref are ignored */
    }

    keyword = ncl_json_obj_get(rule, "type");
    if (!ncl_schema_accepts_type(keyword, value)) {
        char *rendered = ncl_json_render(keyword);
        ncl_schema_error(ctx->errors, path, "expected type: %s, found: %s",
                         rendered != NULL ? rendered : "?",
                         ncl_json_type_label(value));
        ncl_mem_free(rendered);
        return; /* the remaining keywords assume the type matched */
    }

    ncl_validate_enum(ctx, rule, value, path);
    ncl_validate_combinators(ctx, rule, value, path);

    switch (ncl_json_type_of(value)) {
    case NCL_JSON_NUMBER:
        ncl_validate_number(ctx, rule, value, path);
        break;
    case NCL_JSON_STRING:
        ncl_validate_string(ctx, rule, value, path);
        break;
    case NCL_JSON_ARRAY:
        ncl_validate_array(ctx, rule, value, path);
        break;
    case NCL_JSON_OBJECT:
        ncl_validate_object(ctx, rule, value, path);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------- public --- */

/** Walk the schema once and make sure every `pattern` compiles. */
static bool ncl_schema_check_patterns(ncl_schema *schema, const ncl_json *rule,
                                      char **error)
{
    size_t i;

    if (rule == NULL) {
        return true;
    }
    if (ncl_json_type_of(rule) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(rule); i++) {
            if (!ncl_schema_check_patterns(schema, ncl_json_arr_get(rule, i),
                                           error)) {
                return false;
            }
        }
        return true;
    }
    if (ncl_json_type_of(rule) != NCL_JSON_OBJECT) {
        return true;
    }
    {
        const char *pattern = ncl_json_obj_get_string(rule, "pattern");
        if (pattern != NULL && ncl_schema_pattern(schema, pattern) == NULL) {
            ncl_mutex_lock(schema->mutex);
            if (error != NULL) {
                *error = ncl_strdup("unsupported regular expression in `pattern`");
            }
            ncl_mutex_unlock(schema->mutex);
            return false;
        }
    }
    for (i = 0; i < ncl_json_obj_len(rule); i++) {
        const char *key = ncl_json_obj_key_at(rule, i);
        if (key != NULL && strcmp(key, "$ref") == 0) {
            continue;
        }
        if (!ncl_schema_check_patterns(schema, ncl_json_obj_val_at(rule, i),
                                       error)) {
            return false;
        }
    }
    return true;
}

ncl_schema *ncl_schema_compile(const ncl_json *schema, char **error)
{
    ncl_schema *compiled;

    if (error != NULL) {
        *error = NULL;
    }
    if (schema == NULL) {
        if (error != NULL) {
            *error = ncl_strdup("schema is null");
        }
        return NULL;
    }
    if (ncl_json_type_of(schema) != NCL_JSON_OBJECT &&
        ncl_json_type_of(schema) != NCL_JSON_BOOL) {
        if (error != NULL) {
            *error = ncl_strdup("schema must be an object or a boolean");
        }
        return NULL;
    }
    compiled = (ncl_schema *)ncl_mem_calloc(1, sizeof(*compiled));
    if (compiled == NULL) {
        return NULL;
    }
    compiled->root = ncl_json_clone(schema);
    compiled->mutex = ncl_mutex_create();
    ncl_ptrvec_init(&compiled->patterns, ncl_pattern_entry_free);
    if (compiled->root == NULL || compiled->mutex == NULL) {
        ncl_schema_free(compiled);
        return NULL;
    }
    if (!ncl_schema_check_patterns(compiled, compiled->root, error)) {
        ncl_schema_free(compiled);
        return NULL;
    }
    return compiled;
}

ncl_schema *ncl_schema_compile_text(const char *text, size_t len, char **error)
{
    ncl_json *parsed;
    ncl_schema *compiled;

    if (text == NULL) {
        if (error != NULL) {
            *error = ncl_strdup("schema text is null");
        }
        return NULL;
    }
    parsed = ncl_json_parse(text, len, NULL);
    if (parsed == NULL) {
        if (error != NULL) {
            *error = ncl_strdup("schema is not valid JSON");
        }
        return NULL;
    }
    compiled = ncl_schema_compile(parsed, error);
    ncl_json_free(parsed);
    return compiled;
}

void ncl_schema_free(ncl_schema *schema)
{
    if (schema == NULL) {
        return;
    }
    ncl_json_free(schema->root);
    ncl_ptrvec_free(&schema->patterns);
    if (schema->mutex != NULL) {
        ncl_mutex_destroy(schema->mutex);
    }
    ncl_mem_free(schema);
}

const ncl_json *ncl_schema_root(const ncl_schema *schema)
{
    return schema != NULL ? schema->root : NULL;
}

ncl_err ncl_schema_validate(const ncl_schema *schema, const ncl_json *value,
                            ncl_strvec *errors)
{
    ncl_validate_ctx ctx;

    if (schema == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.schema = (ncl_schema *)schema;
    ctx.errors = errors;
    ncl_validate_value(&ctx, schema->root, value, "#");
    return NCL_OK;
}

ncl_err ncl_schema_validate_text(const ncl_schema *schema, const char *text,
                                 size_t len, ncl_strvec *errors)
{
    ncl_json *parsed;
    ncl_err rc;

    if (schema == NULL || text == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    parsed = ncl_json_parse(text, len, NULL);
    if (parsed == NULL) {
        if (errors != NULL) {
            ncl_strvec_push(errors, "校验过程发生错误: 待校验内容不是合法 JSON");
        }
        return NCL_ERR_PARSE;
    }
    rc = ncl_schema_validate(schema, parsed, errors);
    ncl_json_free(parsed);
    return rc;
}

ncl_err ncl_json_schema_validate(const char *json_text, const char *schema_text,
                                 ncl_strvec *errors)
{
    ncl_schema *schema;
    char *error = NULL;
    ncl_err rc;

    if (json_text == NULL || schema_text == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    schema = ncl_schema_compile_text(schema_text, strlen(schema_text), &error);
    if (schema == NULL) {
        if (errors != NULL) {
            ncl_strbuf message;
            ncl_strbuf_init(&message);
            ncl_strbuf_puts(&message, "校验过程发生错误: ");
            ncl_strbuf_puts(&message, error != NULL ? error : "schema 无效");
            ncl_strvec_push(errors, ncl_strbuf_cstr(&message));
            ncl_strbuf_free(&message);
        }
        ncl_mem_free(error);
        return NCL_ERR_PARSE;
    }
    rc = ncl_schema_validate_text(schema, json_text, strlen(json_text), errors);
    ncl_schema_free(schema);
    return rc;
}

char *ncl_schema_join_errors(const ncl_strvec *errors)
{
    ncl_strbuf sb;
    size_t i;
    char *out;

    ncl_strbuf_init(&sb);
    ncl_strbuf_putc(&sb, '[');
    for (i = 0; errors != NULL && i < errors->len; i++) {
        if (i > 0) {
            ncl_strbuf_puts(&sb, ", ");
        }
        ncl_strbuf_puts(&sb, ncl_strvec_at(errors, i));
    }
    ncl_strbuf_putc(&sb, ']');
    out = ncl_strbuf_detach(&sb);
    return out;
}
