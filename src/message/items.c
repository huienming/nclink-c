/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - message item types and the shared params accessors. */
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_message.h"
#include "nclink/ncl_topic.h"

/* ============================================================ type names == */

const char *ncl_msg_type_name(ncl_msg_type type)
{
    switch (type) {
    case NCL_MSG_PING: return "Ping";
    case NCL_MSG_PONG: return "Pong";
    case NCL_MSG_PROBE_VERSION: return "ProbeVersion";
    case NCL_MSG_REGISTER_REQUEST: return "RegisterRequest";
    case NCL_MSG_REGISTER_RESPONSE: return "RegisterResponse";
    case NCL_MSG_QUERY_REQUEST: return "QueryRequest";
    case NCL_MSG_QUERY_RESPONSE: return "QueryResponse";
    case NCL_MSG_SET_REQUEST: return "SetRequest";
    case NCL_MSG_SET_RESPONSE: return "SetResponse";
    case NCL_MSG_PROBE_QUERY_REQUEST: return "ProbeQueryRequest";
    case NCL_MSG_PROBE_QUERY_RESPONSE: return "ProbeQueryResponse";
    case NCL_MSG_PROBE_SET_REQUEST: return "ProbeSetRequest";
    case NCL_MSG_PROBE_SET_RESPONSE: return "ProbeSetResponse";
    case NCL_MSG_SAMPLE: return "Sample";
    case NCL_MSG_EVENT: return "Event";
    case NCL_MSG_METHOD_CALL_REQUEST: return "MethodCallRequest";
    case NCL_MSG_METHOD_CALL_RESPONSE: return "MethodCallResponse";
    default: return "Unknown";
    }
}

ncl_msg_type ncl_msg_type_from_topic(const char *topic)
{
    if (topic == NULL) {
        return NCL_MSG_UNKNOWN;
    }
    /* Order matters: the response prefixes share a stem with the requests. */
    if (ncl_str_starts_with(topic, NCL_TOPIC_QUERY_RESPONSE_PREFIX)) {
        return NCL_MSG_QUERY_RESPONSE;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_QUERY_REQUEST_PREFIX)) {
        return NCL_MSG_QUERY_REQUEST;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_PROBE_QUERY_RESPONSE_PREFIX)) {
        return NCL_MSG_PROBE_QUERY_RESPONSE;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_PROBE_QUERY_REQUEST_PREFIX)) {
        return NCL_MSG_PROBE_QUERY_REQUEST;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_SET_RESPONSE_PREFIX)) {
        return NCL_MSG_SET_RESPONSE;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_SET_REQUEST_PREFIX)) {
        return NCL_MSG_SET_REQUEST;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_PROBE_SET_RESPONSE_PREFIX)) {
        return NCL_MSG_PROBE_SET_RESPONSE;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_PROBE_SET_REQUEST_PREFIX)) {
        return NCL_MSG_PROBE_SET_REQUEST;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_METHOD_CALL_RESPONSE_PREFIX)) {
        return NCL_MSG_METHOD_CALL_RESPONSE;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_METHOD_CALL_REQUEST_PREFIX)) {
        return NCL_MSG_METHOD_CALL_REQUEST;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_PING_PREFIX)) {
        return NCL_MSG_PING;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_PONG_PREFIX)) {
        return NCL_MSG_PONG;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_REGISTER_REQUEST)) {
        return NCL_MSG_REGISTER_REQUEST;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_SAMPLE_PREFIX)) {
        return NCL_MSG_SAMPLE;
    }
    if (ncl_str_starts_with(topic, NCL_TOPIC_EVENT_PREFIX)) {
        return NCL_MSG_EVENT;
    }
    return NCL_MSG_UNKNOWN;
}

/* ========================================================= params helpers = */

const char *ncl_params_operation(const ncl_json *params, const char *fallback)
{
    return ncl_json_obj_get_string(params, "operation") != NULL
               ? ncl_json_obj_get_string(params, "operation")
               : fallback;
}

bool ncl_params_has(const ncl_json *params, const char *key)
{
    return ncl_json_obj_has(params, key);
}

const char *ncl_params_string(const ncl_json *params, const char *key)
{
    return ncl_json_obj_get_string(params, key);
}

bool ncl_params_int(const ncl_json *params, const char *key, long long *out)
{
    return ncl_json_as_int(ncl_json_obj_get(params, key), out);
}

ncl_json *ncl_params_get(const ncl_json *params, const char *key)
{
    return ncl_json_obj_get(params, key);
}

static ncl_err ncl_params_ensure(ncl_json **params)
{
    if (*params == NULL) {
        *params = ncl_json_new_object();
        if (*params == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    return NCL_OK;
}

ncl_err ncl_params_set(ncl_json **params, const char *key, ncl_json *value)
{
    ncl_err rc = ncl_params_ensure(params);
    if (rc != NCL_OK) {
        ncl_json_free(value);
        return rc;
    }
    return ncl_json_obj_set(*params, key, value);
}

ncl_err ncl_params_set_string(ncl_json **params, const char *key, const char *value)
{
    if (value == NULL) {
        return NCL_OK;
    }
    return ncl_params_set(params, key, ncl_json_new_string(value));
}

ncl_err ncl_params_set_int(ncl_json **params, const char *key, long long value)
{
    return ncl_params_set(params, key, ncl_json_new_int(value));
}

ncl_err ncl_params_append_string(ncl_json **params, const char *key, const char *value)
{
    ncl_json *arr;
    ncl_err rc = ncl_params_ensure(params);
    if (rc != NCL_OK) {
        return rc;
    }
    arr = ncl_json_obj_get(*params, key);
    if (arr == NULL) {
        arr = ncl_json_new_array();
        if (arr == NULL) {
            return NCL_ERR_NOMEM;
        }
        if (ncl_json_obj_set(*params, key, arr) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }
    return ncl_json_arr_push(arr, ncl_json_new_string(value));
}

ncl_err ncl_params_indexes(const ncl_json *params, long long **out, size_t *count)
{
    const ncl_json *arr = ncl_json_obj_get(params, "indexes");
    size_t total = 0;
    size_t i;
    long long *result;
    size_t pos = 0;

    if (out == NULL || count == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    *count = 0;
    if (ncl_json_type_of(arr) != NCL_JSON_ARRAY) {
        return NCL_ERR_NOT_FOUND;
    }

    /* Count first: an entry with two '-' separated parts contributes both bounds. */
    for (i = 0; i < ncl_json_arr_len(arr); i++) {
        char *text = ncl_json_as_text(ncl_json_arr_get(arr, i));
        if (text == NULL) {
            continue;
        }
        total += strchr(text, '-') != NULL ? 2 : 1;
        ncl_mem_free(text);
    }
    if (total == 0) {
        return NCL_ERR_NOT_FOUND;
    }

    result = (long long *)ncl_mem_alloc(total * sizeof(long long));
    if (result == NULL) {
        return NCL_ERR_NOMEM;
    }

    for (i = 0; i < ncl_json_arr_len(arr); i++) {
        char *text = ncl_json_as_text(ncl_json_arr_get(arr, i));
        char *dash;
        if (text == NULL) {
            continue;
        }
        dash = strchr(text, '-');
        if (dash != NULL) {
            *dash = '\0';
            result[pos++] = strtoll(text, NULL, 10);
            result[pos++] = strtoll(dash + 1, NULL, 10);
        } else {
            result[pos++] = strtoll(text, NULL, 10);
        }
        ncl_mem_free(text);
    }
    *out = result;
    *count = pos;
    return NCL_OK;
}

/* ==================================================== query request item == */

ncl_query_request_item *ncl_query_request_item_new(const char *id)
{
    ncl_query_request_item *item =
        (ncl_query_request_item *)ncl_mem_calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    if (id != NULL) {
        item->id = ncl_strdup(id);
        if (item->id == NULL) {
            ncl_mem_free(item);
            return NULL;
        }
    }
    return item;
}

void ncl_query_request_item_free(ncl_query_request_item *item)
{
    if (item == NULL) {
        return;
    }
    ncl_mem_free(item->id);
    ncl_json_free(item->params);
    ncl_mem_free(item);
}

bool ncl_query_request_item_is_valid(const ncl_query_request_item *item)
{
    return item != NULL && !ncl_str_is_empty(item->id);
}

const char *ncl_query_request_item_operation(const ncl_query_request_item *item)
{
    if (item == NULL) {
        return NULL;
    }
    return ncl_params_operation(item->params, "get_value");
}

ncl_err ncl_query_request_item_indexes(const ncl_query_request_item *item,
                                       long long **out, size_t *count)
{
    if (item == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_params_indexes(item->params, out, count);
}

/* =================================================== query response item == */

ncl_query_response_item *ncl_query_response_item_new(const char *id)
{
    ncl_query_response_item *item =
        (ncl_query_response_item *)ncl_mem_calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    item->values = ncl_json_new_array();
    if (item->values == NULL) {
        ncl_mem_free(item);
        return NULL;
    }
    if (id != NULL) {
        item->id = ncl_strdup(id);
        if (item->id == NULL) {
            ncl_query_response_item_free(item);
            return NULL;
        }
    }
    return item;
}

void ncl_query_response_item_free(ncl_query_response_item *item)
{
    if (item == NULL) {
        return;
    }
    ncl_mem_free(item->id);
    ncl_mem_free(item->code);
    ncl_mem_free(item->reason);
    ncl_json_free(item->params);
    ncl_json_free(item->values);
    ncl_mem_free(item);
}

bool ncl_query_response_item_is_valid(const ncl_query_response_item *item)
{
    return item != NULL && !ncl_str_is_empty(item->id) &&
           ncl_check_is_code_valid(item->code);
}

const char *ncl_query_response_item_operation(const ncl_query_response_item *item)
{
    if (item == NULL) {
        return NULL;
    }
    return ncl_params_operation(item->params, "get_value");
}

bool ncl_query_response_item_has_data(const ncl_query_response_item *item)
{
    if (item == NULL || item->code == NULL ||
        strcmp(item->code, NCL_KW_CODE_NG) == 0) {
        return false;
    }
    return ncl_json_arr_len(item->values) > 0;
}

ncl_json *ncl_query_response_item_data(const ncl_query_response_item *item)
{
    if (item == NULL || ncl_json_arr_len(item->values) == 0) {
        return NULL;
    }
    return ncl_json_arr_get(item->values, 0);
}

ncl_err ncl_query_response_item_add_value(ncl_query_response_item *item,
                                          ncl_json *value)
{
    if (item == NULL) {
        ncl_json_free(value);
        return NCL_ERR_INVALID_ARG;
    }
    if (item->values == NULL) {
        item->values = ncl_json_new_array();
        if (item->values == NULL) {
            ncl_json_free(value);
            return NCL_ERR_NOMEM;
        }
    }
    return ncl_json_arr_push(item->values, value);
}

/* True when the response item answers the request item. */
bool ncl_query_response_item_matches(const ncl_query_response_item *item,
                                     const ncl_query_request_item *request)
{
    const char *request_op;
    const char *response_op;
    long long request_int = 0;
    long long response_int = 0;
    bool request_has;
    bool response_has;
    long long *request_indexes = NULL;
    size_t request_index_count = 0;
    long long *response_indexes = NULL;
    size_t response_index_count = 0;
    ncl_json *request_keys;
    ncl_json *response_keys;
    bool matched = true;

    if (item == NULL || request == NULL) {
        return false;
    }
    if (item->id == NULL || request->id == NULL ||
        strcmp(item->id, request->id) != 0) {
        return false;
    }

    request_op = ncl_query_request_item_operation(request);
    response_op = ncl_query_response_item_operation(item);
    if (request_op == NULL) {
        if (response_op != NULL && strcmp(response_op, "set_value") != 0) {
            return false;
        }
    } else if (response_op == NULL || strcmp(request_op, response_op) != 0) {
        return false;
    }

    /* length */
    request_has = ncl_params_int(request->params, "length", &request_int);
    response_has = ncl_params_int(item->params, "length", &response_int);
    if (request_has != response_has ||
        (request_has && request_int != response_int)) {
        return false;
    }

    /* offset: the response offset is compared against the request length */
    {
        long long request_length = 0;
        bool has_request_length = ncl_params_int(request->params, "length", &request_length);
        request_has = ncl_params_int(request->params, "offset", &request_int);
        response_has = ncl_params_int(item->params, "offset", &response_int);
        if (request_has != response_has ||
            (request_has && has_request_length && request_length != response_int)) {
            return false;
        }
    }

    /* indexes */
    request_has = ncl_query_request_item_indexes(request, &request_indexes,
                                                 &request_index_count) == NCL_OK;
    response_has = ncl_params_indexes(item->params, &response_indexes,
                                      &response_index_count) == NCL_OK;
    if (request_has != response_has) {
        matched = false;
    } else if (request_has) {
        if (request_index_count != response_index_count) {
            matched = false;
        } else {
            size_t i;
            for (i = 0; i < request_index_count; i++) {
                if (request_indexes[i] != response_indexes[i]) {
                    matched = false;
                    break;
                }
            }
        }
    }
    ncl_mem_free(request_indexes);
    ncl_mem_free(response_indexes);
    if (!matched) {
        return false;
    }

    /* keys */
    request_keys = ncl_params_get(request->params, "keys");
    response_keys = ncl_params_get(item->params, "keys");
    if ((request_keys == NULL) != (response_keys == NULL)) {
        return false;
    }
    if (request_keys != NULL && !ncl_json_equals(request_keys, response_keys)) {
        return false;
    }
    return true;
}

/* ====================================================== set request item == */

ncl_set_request_item *ncl_set_request_item_new(const char *id)
{
    ncl_set_request_item *item = (ncl_set_request_item *)ncl_mem_calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    if (id != NULL) {
        item->id = ncl_strdup(id);
        if (item->id == NULL) {
            ncl_mem_free(item);
            return NULL;
        }
    }
    return item;
}

void ncl_set_request_item_free(ncl_set_request_item *item)
{
    if (item == NULL) {
        return;
    }
    ncl_mem_free(item->id);
    ncl_json_free(item->params);
    ncl_mem_free(item);
}

bool ncl_set_request_item_is_valid(const ncl_set_request_item *item)
{
    return item != NULL && !ncl_str_is_empty(item->id);
}

const char *ncl_set_request_item_operation(const ncl_set_request_item *item)
{
    if (item == NULL) {
        return NULL;
    }
    return ncl_params_operation(item->params, "set_value");
}

/* ===================================================== set response item == */

ncl_set_response_item *ncl_set_response_item_new(const char *id)
{
    ncl_set_response_item *item = (ncl_set_response_item *)ncl_mem_calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    if (id != NULL) {
        item->id = ncl_strdup(id);
        if (item->id == NULL) {
            ncl_mem_free(item);
            return NULL;
        }
    }
    return item;
}

void ncl_set_response_item_free(ncl_set_response_item *item)
{
    if (item == NULL) {
        return;
    }
    ncl_mem_free(item->id);
    ncl_mem_free(item->code);
    ncl_mem_free(item->reason);
    ncl_json_free(item->params);
    ncl_json_free(item->result);
    ncl_mem_free(item);
}

bool ncl_set_response_item_is_valid(const ncl_set_response_item *item)
{
    return item != NULL && !ncl_str_is_empty(item->id) &&
           ncl_check_is_code_valid(item->code);
}

bool ncl_set_response_item_has_error(const ncl_set_response_item *item)
{
    /* True unless the item reports "NG". */
    return item == NULL || !ncl_check_is_code_ng(item->code);
}

bool ncl_set_response_item_matches(const ncl_set_response_item *item,
                                   const ncl_set_request_item *request)
{
    const char *request_op;
    const char *response_op;
    long long request_int = 0;
    long long response_int = 0;
    bool request_has;
    bool response_has;

    if (item == NULL || request == NULL) {
        return false;
    }
    if (item->id == NULL || request->id == NULL ||
        strcmp(item->id, request->id) != 0) {
        return false;
    }

    request_op = ncl_set_request_item_operation(request);
    response_op = ncl_params_operation(item->params, "set_value");
    if (request_op == NULL) {
        if (response_op != NULL && strcmp(response_op, "set_value") != 0) {
            return false;
        }
    } else if (response_op == NULL || strcmp(request_op, response_op) != 0) {
        return false;
    }

    request_has = ncl_params_int(request->params, "length", &request_int);
    response_has = ncl_params_int(item->params, "length", &response_int);
    if (request_has != response_has || (request_has && request_int != response_int)) {
        return false;
    }

    {
        long long request_length = 0;
        bool has_request_length = ncl_params_int(request->params, "length", &request_length);
        request_has = ncl_params_int(request->params, "offset", &request_int);
        response_has = ncl_params_int(item->params, "offset", &response_int);
        if (request_has != response_has ||
            (request_has && has_request_length && request_length != response_int)) {
            return false;
        }
    }

    request_has = ncl_params_int(request->params, "index", &request_int);
    response_has = ncl_params_int(item->params, "index", &response_int);
    if (request_has != response_has || (request_has && request_int != response_int)) {
        return false;
    }

    {
        const char *request_key = ncl_params_string(request->params, "key");
        const char *response_key = ncl_params_string(item->params, "key");
        if ((request_key == NULL) != (response_key == NULL)) {
            return false;
        }
        if (request_key != NULL && strcmp(request_key, response_key) != 0) {
            return false;
        }
    }
    return true;
}

/* ============================================================== sample === */

ncl_sample_item *ncl_sample_item_new(void)
{
    ncl_sample_item *item = (ncl_sample_item *)ncl_mem_calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    item->data = ncl_json_new_array();
    if (item->data == NULL) {
        ncl_mem_free(item);
        return NULL;
    }
    return item;
}

void ncl_sample_item_free(ncl_sample_item *item)
{
    if (item == NULL) {
        return;
    }
    ncl_mem_free(item->encoding);
    ncl_json_free(item->data);
    ncl_mem_free(item);
}

ncl_sample_item *ncl_sample_item_clone(const ncl_sample_item *item)
{
    ncl_sample_item *copy;
    if (item == NULL) {
        return NULL;
    }
    copy = ncl_sample_item_new();
    if (copy == NULL) {
        return NULL;
    }
    if (item->encoding != NULL) {
        copy->encoding = ncl_strdup(item->encoding);
    }
    ncl_json_free(copy->data);
    copy->data = ncl_json_clone(item->data);
    if (copy->data == NULL) {
        ncl_sample_item_free(copy);
        return NULL;
    }
    return copy;
}

bool ncl_sample_item_is_valid(const ncl_sample_item *item)
{
    return item != NULL; /* A sample item is valid unconditionally. */
}

ncl_err ncl_sample_item_add_value(ncl_sample_item *item, ncl_json *value)
{
    if (item == NULL) {
        ncl_json_free(value);
        return NCL_ERR_INVALID_ARG;
    }
    if (item->data == NULL) {
        item->data = ncl_json_new_array();
        if (item->data == NULL) {
            ncl_json_free(value);
            return NCL_ERR_NOMEM;
        }
    }
    return ncl_json_arr_push(item->data, value);
}

bool ncl_sample_item_is_same(const ncl_sample_item *a, const ncl_sample_item *b)
{
    size_t i;
    if (a == NULL || b == NULL) {
        return false;
    }
    if (ncl_json_arr_len(a->data) != ncl_json_arr_len(b->data)) {
        return false;
    }
    for (i = 0; i < ncl_json_arr_len(a->data); i++) {
        if (!ncl_json_equals(ncl_json_arr_get(a->data, i),
                             ncl_json_arr_get(b->data, i))) {
            return false;
        }
    }
    return true;
}
