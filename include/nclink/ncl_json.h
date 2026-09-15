/*
 * NC-Link core - minimal JSON DOM with ordered object keys.
 *
 * Behaviour that the protocol writers rely on:
 *  - object keys keep their insertion order, so serialisation is deterministic,
 *  - null valued entries are omitted by the caller (see NCL_JSON_OMIT),
 *  - unknown properties are ignored while parsing,
 *  - output is compact, raw UTF-8, with no whitespace between tokens.
 */
#ifndef NCL_JSON_H
#define NCL_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum nesting depth accepted by the parser (stack safety). */
#define NCL_JSON_MAX_DEPTH 200

typedef enum {
    NCL_JSON_NULL = 0,
    NCL_JSON_BOOL,
    NCL_JSON_NUMBER, /**< @see ncl_json_number_raw for the original literal */
    NCL_JSON_STRING,
    NCL_JSON_ARRAY,
    NCL_JSON_OBJECT
} ncl_json_type;

typedef struct ncl_json ncl_json;

/* ------------------------------------------------------------ life cycle -- */

ncl_json *ncl_json_new_null(void);
ncl_json *ncl_json_new_bool(bool value);
ncl_json *ncl_json_new_int(long long value);
ncl_json *ncl_json_new_double(double value);
ncl_json *ncl_json_new_string(const char *value);
ncl_json *ncl_json_new_string_len(const char *value, size_t len);
ncl_json *ncl_json_new_array(void);
ncl_json *ncl_json_new_object(void);
ncl_json *ncl_json_clone(const ncl_json *j);
void      ncl_json_free(ncl_json *j);

ncl_json_type ncl_json_type_of(const ncl_json *j);
bool          ncl_json_is_null(const ncl_json *j);
bool          ncl_json_is_number(const ncl_json *j);

/* ---------------------------------------------------------------- object -- */

/**
 * Insert or replace @p key. Takes ownership of @p value in every case (it is
 * released when the call fails). Passing NULL for @p value removes the key.
 */
ncl_err ncl_json_obj_set(ncl_json *obj, const char *key, ncl_json *value);

ncl_err ncl_json_obj_set_string(ncl_json *obj, const char *key, const char *value);
ncl_err ncl_json_obj_set_int(ncl_json *obj, const char *key, long long value);
ncl_err ncl_json_obj_set_double(ncl_json *obj, const char *key, double value);
ncl_err ncl_json_obj_set_bool(ncl_json *obj, const char *key, bool value);
ncl_err ncl_json_obj_set_null(ncl_json *obj, const char *key);

ncl_json *ncl_json_obj_get(const ncl_json *obj, const char *key);
bool      ncl_json_obj_has(const ncl_json *obj, const char *key);
size_t    ncl_json_obj_len(const ncl_json *obj);
const char *ncl_json_obj_key_at(const ncl_json *obj, size_t index);
ncl_json   *ncl_json_obj_val_at(const ncl_json *obj, size_t index);
ncl_err     ncl_json_obj_remove(ncl_json *obj, const char *key);

/**
 * Convenience getters. They return the supplied default when the key is
 * missing, JSON null, or of an incompatible type.
 */
const char *ncl_json_obj_get_string(const ncl_json *obj, const char *key);
long long   ncl_json_obj_get_int(const ncl_json *obj, const char *key, long long def);
double      ncl_json_obj_get_double(const ncl_json *obj, const char *key, double def);
bool        ncl_json_obj_get_bool(const ncl_json *obj, const char *key, bool def);

/* ----------------------------------------------------------------- array -- */

/** Append @p value; ownership transfers to @p arr in every case. */
ncl_err ncl_json_arr_push(ncl_json *arr, ncl_json *value);
ncl_json *ncl_json_arr_get(const ncl_json *arr, size_t index);
size_t    ncl_json_arr_len(const ncl_json *arr);
ncl_json *ncl_json_arr_take(ncl_json *arr, size_t index);

/* ---------------------------------------------------------------- scalar -- */

/** String payload, or NULL when @p j is not a JSON string. */
const char *ncl_json_as_string(const ncl_json *j);

/** Raw number literal as written in the source document. */
const char *ncl_json_number_raw(const ncl_json *j);

/** True when the value is a number, or a string holding a decimal integer. */
bool ncl_json_as_int(const ncl_json *j, long long *out);

/** True when the value is a number, or a string holding a finite number. */
bool ncl_json_as_double(const ncl_json *j, double *out);

bool ncl_json_as_bool(const ncl_json *j, bool *out);

/**
 * Textual representation of a scalar value, used by the request parsers
 * (e.g. an "offset" that arrives as a string or as a number).
 * Returns a heap string for numbers/booleans and the literal text for strings.
 * NULL for objects/arrays/null.
 */
char *ncl_json_as_text(const ncl_json *j);

/* ------------------------------------------------------------ parse/write -- */

/**
 * Parse a JSON document. Trailing whitespace is allowed, trailing garbage is
 * not. On failure NULL is returned and, when @p err is not NULL, a diagnostic
 * message is appended to it.
 */
ncl_json *ncl_json_parse(const char *text, size_t len, ncl_strbuf *err);
ncl_json *ncl_json_parse_cstr(const char *text, ncl_strbuf *err);

/** Serialise compactly into @p out. */
ncl_err ncl_json_write(const ncl_json *j, ncl_strbuf *out);

/** Serialise into a freshly allocated string. */
char *ncl_json_write_string(const ncl_json *j);

/** Deep structural equality (object key order is not significant). */
bool ncl_json_equals(const ncl_json *a, const ncl_json *b);

/* ------------------------------------------------------------- helpers -- */

/**
 * Serialise a string vector (ncl_common.h) as a JSON array of strings.
 * Returns a new array the caller owns, or NULL on allocation failure.
 */
ncl_json *ncl_strvec_to_json(const ncl_strvec *v);

#ifdef __cplusplus
}
#endif

#endif /* NCL_JSON_H */
