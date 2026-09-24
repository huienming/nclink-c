/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - constants, enumerations and validity helpers. */
#include "nclink/ncl_general.h"

#include <string.h>

const char *ncl_code_to_string(ncl_code code)
{
    switch (code) {
    case NCL_CODE_OK: return NCL_KW_CODE_OK;
    case NCL_CODE_NG: return NCL_KW_CODE_NG;
    case NCL_CODE_PENDING: return NCL_KW_CODE_PENDING;
    default: return NCL_KW_CODE_NG;
    }
}

bool ncl_code_parse(const char *text, ncl_code *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcmp(text, NCL_KW_CODE_OK) == 0) {
        *out = NCL_CODE_OK;
        return true;
    }
    if (strcmp(text, NCL_KW_CODE_NG) == 0) {
        *out = NCL_CODE_NG;
        return true;
    }
    if (strcmp(text, NCL_KW_CODE_PENDING) == 0) {
        *out = NCL_CODE_PENDING;
        return true;
    }
    return false;
}

const char *ncl_operation_to_string(ncl_operation op)
{
    switch (op) {
    case NCL_OP_GET_VALUE: return "get_value";
    case NCL_OP_GET_LENGTH: return "get_length";
    case NCL_OP_GET_KEYS: return "get_keys";
    case NCL_OP_GET_ATTRIBUTES: return "get_attributes";
    case NCL_OP_SET_VALUE: return "set_value";
    case NCL_OP_ADD: return "add";
    case NCL_OP_DELETE: return "delete";
    case NCL_OP_FUNC_CALL: return "call";
    case NCL_OP_FUNC_STATUS: return "status";
    case NCL_OP_FUNC_RESULT: return "result";
    case NCL_OP_FUNC_CANCEL: return "cancel";
    default: return "";
    }
}

bool ncl_operation_parse(const char *text, ncl_operation *out)
{
    int i;
    if (text == NULL || out == NULL) {
        return false;
    }
    for (i = 0; i <= (int)NCL_OP_FUNC_CANCEL; i++) {
        if (strcmp(text, ncl_operation_to_string((ncl_operation)i)) == 0) {
            *out = (ncl_operation)i;
            return true;
        }
    }
    return false;
}

bool ncl_check_is_code_valid(const char *code)
{
    if (code == NULL) {
        return false;
    }
    return ncl_streq_ignore_case(code, NCL_KW_CODE_NG) ||
           ncl_streq_ignore_case(code, NCL_KW_CODE_OK);
}

bool ncl_check_is_code_ok(const char *code)
{
    return code != NULL && ncl_streq_ignore_case(code, NCL_KW_CODE_OK);
}

bool ncl_check_is_code_ng(const char *code)
{
    return code != NULL && ncl_streq_ignore_case(code, NCL_KW_CODE_NG);
}

bool ncl_check_is_pending(const char *code)
{
    return code != NULL && ncl_streq_ignore_case(code, NCL_KW_CODE_PENDING);
}

bool ncl_check_is_data_type_valid(const char *data_type)
{
    if (data_type == NULL) {
        return true;
    }
    return ncl_streq_ignore_case(data_type, NCL_DATA_TYPE_HASH) ||
           ncl_streq_ignore_case(data_type, NCL_DATA_TYPE_LIST);
}
