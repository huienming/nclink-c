/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - common types: error codes, string helpers, string buffer,
 * pointer vector.
 *
 * Building blocks shared by every module: error codes, string helpers, a
 * growable text buffer and owning vectors.
 */
#ifndef NCL_COMMON_H
#define NCL_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Library version. The NC-Link specification revision implemented by this
 * library is still 3.0.0 (GB/T 41970-2022).
 */
#define NCL_VERSION "3.1.0"

/* ------------------------------------------------------------------ error -- */
typedef int ncl_err;

#define NCL_OK 0

/* Infrastructure errors. */
#define NCL_ERR              (-1)  /**< generic failure            */
#define NCL_ERR_NOMEM        (-2)  /**< allocation failure         */
#define NCL_ERR_PARSE        (-3)  /**< malformed JSON / payload   */
#define NCL_ERR_TIMEOUT      (-4)  /**< request timed out          */
#define NCL_ERR_IO           (-5)  /**< file or socket error       */
#define NCL_ERR_NOT_FOUND    (-6)  /**< lookup miss                */
#define NCL_ERR_EXISTS       (-7)  /**< duplicate entry            */
#define NCL_ERR_NOT_SUPPORTED (-8) /**< feature not compiled in    */
#define NCL_ERR_INVALID_ARG  (-9)  /**< caller passed a bad pointer/enum */
#define NCL_ERR_STATE        (-10) /**< object not in a usable state */
#define NCL_ERR_RANGE        (-11) /**< index outside bounds       */
#define NCL_ERR_CONNECT      (-12) /**< transport connect failed   */
#define NCL_ERR_CLOSED       (-13) /**< object already closed      */

/* Domain validation errors, one per NC-Link validity rule. */
#define NCL_ERR_INVALID_CODE        (-100) /**< InvalidCodeException        */
#define NCL_ERR_INVALID_DATA_NAME   (-101) /**< InvalidDataNameException    */
#define NCL_ERR_INVALID_DATA_TYPE   (-102) /**< InvalidDataTypException     */
#define NCL_ERR_INVALID_DEVICE_ID   (-103) /**< InvalidDeviceIdException    */
#define NCL_ERR_INVALID_ENCODING    (-104) /**< InvalidEncodingException    */
#define NCL_ERR_INVALID_ID          (-105) /**< InvalidIdException          */
#define NCL_ERR_INVALID_INDEX_RANGE (-106) /**< InvalidIndexRangeException  */
#define NCL_ERR_INVALID_ITEM        (-107) /**< InvalidItemException        */
#define NCL_ERR_INVALID_KEY         (-108) /**< InvalidKeyException         */
#define NCL_ERR_INVALID_MESSAGE     (-109) /**< InvalidMessageException     */
#define NCL_ERR_INVALID_MESSAGE_ID  (-110) /**< InvalidMessageIdException   */
#define NCL_ERR_INVALID_MODEL       (-111) /**< InvalidModelException       */
#define NCL_ERR_INVALID_NODE        (-112) /**< InvalidNodeException        */
#define NCL_ERR_INVALID_NUMBER      (-113) /**< InvalidNumberException      */
#define NCL_ERR_INVALID_REQUEST     (-114) /**< InvalidRequestException     */
#define NCL_ERR_INVALID_TYPE        (-115) /**< InvalidTypeException        */
#define NCL_ERR_INVALID_VALUE       (-116) /**< InvalidValueException       */
#define NCL_ERR_INVALID_VERSION     (-117) /**< InvalidVersionException     */

/**
 * Stable, human readable name of an error code. The names are part of the
 * public API: log lines and REST failure messages quote them verbatim.
 */
const char *ncl_err_name(ncl_err err);

/* ------------------------------------------------------------ string utils -- */

/** Heap copy of @p s (NULL safe). Caller frees. */
char *ncl_strdup(const char *s);

/** Heap copy of the first @p len bytes of @p s (NUL terminated). */
char *ncl_strndup(const char *s, size_t len);

/**
 * printf into a freshly allocated buffer. *out is always set to a heap string
 * or NULL on failure. Returns NCL_OK / NCL_ERR_NOMEM.
 */
ncl_err ncl_asprintf(char **out, const char *fmt, ...);
ncl_err ncl_vasprintf(char **out, const char *fmt, va_list ap);

/** Case-insensitive ASCII equality. */
bool ncl_streq_ignore_case(const char *a, const char *b);

bool ncl_str_starts_with(const char *s, const char *prefix);
bool ncl_str_ends_with(const char *s, const char *suffix);

/** True when @p s is NULL or empty. */
bool ncl_str_is_empty(const char *s);

/** True when @p s is NULL, empty, or contains only whitespace. */
bool ncl_str_is_blank(const char *s);

/**
 * Duplicate @p s with leading and trailing ASCII whitespace removed.
 */
char *ncl_str_trim_dup(const char *s);

/** Free a heap pointer and set the variable to NULL. */
void ncl_free_safe(void *ptr);

/**
 * Format a random version 4 UUID into @p out in lower case (8-4-4-4-12).
 * @p out must hold at least 37 bytes. Returns NCL_OK / NCL_ERR.
 */
ncl_err ncl_uuid4(char *out, size_t out_len);

/* ------------------------------------------------------------- ncl_strbuf -- */

/** Growable byte/string buffer used by every writer in the library. */
typedef struct {
    char  *data; /**< always NUL terminated once non-NULL */
    size_t len;  /**< length excluding the terminator    */
    size_t cap;  /**< allocated capacity including room for NUL */
} ncl_strbuf;

void   ncl_strbuf_init(ncl_strbuf *sb);
void   ncl_strbuf_free(ncl_strbuf *sb);
void   ncl_strbuf_reset(ncl_strbuf *sb);
ncl_err ncl_strbuf_reserve(ncl_strbuf *sb, size_t additional);
ncl_err ncl_strbuf_append(ncl_strbuf *sb, const char *data, size_t len);
ncl_err ncl_strbuf_puts(ncl_strbuf *sb, const char *s);
ncl_err ncl_strbuf_putc(ncl_strbuf *sb, char c);
ncl_err ncl_strbuf_printf(ncl_strbuf *sb, const char *fmt, ...);

/** Detach the buffer contents; caller frees. Resets @p sb to empty. */
char *ncl_strbuf_detach(ncl_strbuf *sb);

/** NUL terminated view of the buffer (never NULL for an initialised buffer). */
const char *ncl_strbuf_cstr(ncl_strbuf *sb);

/* ---------------------------------------------------------------- ptrvec -- */

/**
 * Vector of owned pointers with an optional element destructor, used for the
 * list valued fields of the message and model objects.
 */
typedef void (*ncl_free_fn)(void *element);

typedef struct {
    void      **items;
    size_t      len;
    size_t      cap;
    ncl_free_fn free_fn; /**< may be NULL when elements are not owned */
} ncl_ptrvec;

void   ncl_ptrvec_init(ncl_ptrvec *v, ncl_free_fn free_fn);
void   ncl_ptrvec_free(ncl_ptrvec *v);
void   ncl_ptrvec_clear(ncl_ptrvec *v);
ncl_err ncl_ptrvec_push(ncl_ptrvec *v, void *item);
ncl_err ncl_ptrvec_push_owned(ncl_ptrvec *v, void *item);
void  *ncl_ptrvec_at(const ncl_ptrvec *v, size_t index);
size_t ncl_ptrvec_len(const ncl_ptrvec *v);
/** Detach ownership of element @p index; slot is removed. */
void  *ncl_ptrvec_take(ncl_ptrvec *v, size_t index);

/* --------------------------------------------------------------- strvec -- */

/** Vector of owned C strings. */
typedef struct {
    char  **items;
    size_t  len;
    size_t  cap;
} ncl_strvec;

void    ncl_strvec_init(ncl_strvec *v);
void    ncl_strvec_free(ncl_strvec *v);
void    ncl_strvec_clear(ncl_strvec *v);
/** Copies @p s; returns NCL_OK / NCL_ERR_NOMEM. */
ncl_err ncl_strvec_push(ncl_strvec *v, const char *s);
const char *ncl_strvec_at(const ncl_strvec *v, size_t index);
size_t  ncl_strvec_len(const ncl_strvec *v);
bool    ncl_strvec_contains(const ncl_strvec *v, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* NCL_COMMON_H */
