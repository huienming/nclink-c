/*
 * NC-Link core - general constants, enumerations and validation helpers.
 *
 * The literals, enumerations and validity helpers of NC-Link: path separators,
 * status keywords, data type keywords, codes and operations. The protocol level
 * validity checks live next to the objects they validate (see ncl_message.h).
 */
#ifndef NCL_GENERAL_H
#define NCL_GENERAL_H

#include <stdbool.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- topic/constants -- */

/** Path component separators (Constants). */
#define NCL_PATH_SEPARATOR "/"
#define NCL_PATH_TAG_SEPARATOR "@"
#define NCL_PATH_TYPE_SEPARATOR "/"
#define NCL_PATH_ELEMENT_SEPARATOR "@"

/** Status keywords. */
#define NCL_KW_CODE_OK "OK"
#define NCL_KW_CODE_NG "NG"
#define NCL_KW_CODE_PENDING "PENDING"

/** Data type keywords. */
#define NCL_DATA_TYPE_LIST "LIST"
#define NCL_DATA_TYPE_HASH "HASH"
#define NCL_DATA_TYPE_SEPARATOR "$"
#define NCL_DATA_CHILD_SEPARATOR "-"
#define NCL_OPERATION_SEPARATOR "#"

/** Default timeout of one protocol operation, milliseconds. */
#define NCL_OPERATION_TIMEOUT 5000

/* -------------------------------------------------------------- Code ----- */

/** Status code of a request or response item ("code"). */
typedef enum {
    NCL_CODE_OK = 0,
    NCL_CODE_NG,
    NCL_CODE_PENDING
} ncl_code;

/** Keyword of @p code: "OK", "NG" or "PENDING". */
const char *ncl_code_to_string(ncl_code code);

/** Parse "OK" / "NG" / "PENDING" (case sensitive). */
bool ncl_code_parse(const char *text, ncl_code *out);

/* --------------------------------------------------------- Operation ----- */

/** Operation part of a path binding ("<operation>#<path>"). */
typedef enum {
    NCL_OP_GET_VALUE = 0,
    NCL_OP_GET_LENGTH,
    NCL_OP_GET_KEYS,
    NCL_OP_GET_ATTRIBUTES,
    NCL_OP_SET_VALUE,
    NCL_OP_ADD,
    NCL_OP_DELETE,
    NCL_OP_FUNC_CALL,
    NCL_OP_FUNC_STATUS,
    NCL_OP_FUNC_RESULT,
    NCL_OP_FUNC_CANCEL
} ncl_operation;

/** Keyword of @p op, e.g. "get_value" or "set_value". */
const char *ncl_operation_to_string(ncl_operation op);

/** Parse an operation name; false when unknown. */
bool ncl_operation_parse(const char *text, ncl_operation *out);

/* ------------------------------------------------------ validity helpers -- */

/** True when @p code is "OK" or "NG" (case insensitive); NULL is invalid. */
bool ncl_check_is_code_valid(const char *code);

/** True when @p code is "OK". */
bool ncl_check_is_code_ok(const char *code);

/** True when @p code is "NG". */
bool ncl_check_is_code_ng(const char *code);

/** True when @p code is "PENDING". */
bool ncl_check_is_pending(const char *code);

/** True when @p data_type is NULL (no suffix), "LIST" or "HASH". */
bool ncl_check_is_data_type_valid(const char *data_type);

#ifdef __cplusplus
}
#endif

#endif /* NCL_GENERAL_H */
