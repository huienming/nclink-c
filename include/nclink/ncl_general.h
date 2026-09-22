/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

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

/* "status" of a Method/Status/Response (asynchronous method call). */
#define NCL_KW_STATUS_EXECUTING "executing"
#define NCL_KW_STATUS_WAITING   "waiting"
#define NCL_KW_STATUS_STOPPED   "stopped"
#define NCL_KW_STATUS_SLEEP     "sleep"

/* "result" of a Method/Result/Response (asynchronous method call). */
#define NCL_KW_RESULT_FINISHED "finished"
#define NCL_KW_RESULT_CANCEL   "cancel"
#define NCL_KW_RESULT_ERROR    "error"

/** Data type keywords. */
#define NCL_DATA_TYPE_LIST "LIST"
#define NCL_DATA_TYPE_HASH "HASH"
#define NCL_DATA_TYPE_SEPARATOR "$"
#define NCL_DATA_CHILD_SEPARATOR "-"
#define NCL_OPERATION_SEPARATOR "#"

/** Default timeout of one protocol operation, milliseconds. */
#define NCL_OPERATION_TIMEOUT 5000

/* ------------------------------------------------------- the METHODS item -- */

/**
 * The model item that lists every callable method. It is a root level config
 * of the device model: id "methods", type "METHODS", dataType LIST and a
 * "value" array with one object per method (tool / method / address / params
 * schema / result schema / the model paths it serves).
 *
 * The device side writes it (ncl_server_refresh_methods()) so that a Probe
 * answer carries the whole capability surface, and the client side reads it
 * (ncl_client_methods()).
 */
#define NCL_METHODS_NODE_ID   "methods"
#define NCL_METHODS_NODE_TYPE "METHODS"
#define NCL_METHODS_PATH      "/METHODS"

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

/** How many operations there are - a point holds one bit each. */
#define NCL_OP_COUNT ((unsigned)NCL_OP_FUNC_CANCEL + 1u)

/** Bit of @p op, for a set of operations (see ncl_tool_point.ops). */
#define NCL_OP_BIT(op) (1u << (unsigned)(op))

/**
 * The operations of the standard's "Query" instruction (册 5 §5.2.7):
 * get_value / get_length / get_keys / get_attributes.
 */
#define NCL_OP_QUERY_MASK                                              \
    (NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_GET_LENGTH) |    \
     NCL_OP_BIT(NCL_OP_GET_KEYS) | NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

/**
 * The operations of the standard's "Set" instruction (册 5 §5.2.8):
 * set_value / add / delete. Every one of them changes something, so a
 * point that answers one of them has to be readable too.
 */
#define NCL_OP_WRITE_MASK                                              \
    (NCL_OP_BIT(NCL_OP_SET_VALUE) | NCL_OP_BIT(NCL_OP_ADD) |          \
     NCL_OP_BIT(NCL_OP_DELETE))

/** The call family: a method call and the status / result / cancel it has. */
#define NCL_OP_CALL_MASK                                               \
    (NCL_OP_BIT(NCL_OP_FUNC_CALL) | NCL_OP_BIT(NCL_OP_FUNC_STATUS) |  \
     NCL_OP_BIT(NCL_OP_FUNC_RESULT) | NCL_OP_BIT(NCL_OP_FUNC_CANCEL))

/** Everything that reaches a value: a point with none of them is a method. */
#define NCL_OP_VALUE_MASK (NCL_OP_QUERY_MASK | NCL_OP_WRITE_MASK)

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
