/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - message container: life cycle, validation, JSON both ways. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_platform.h"

/* ---------------------------------------------------------- destructors --- */

static void ncl_item_dtor_query_request(void *p)
{
    ncl_query_request_item_free((ncl_query_request_item *)p);
}

static void ncl_item_dtor_query_response(void *p)
{
    ncl_query_response_item_free((ncl_query_response_item *)p);
}

static void ncl_item_dtor_set_request(void *p)
{
    ncl_set_request_item_free((ncl_set_request_item *)p);
}

static void ncl_item_dtor_set_response(void *p)
{
    ncl_set_response_item_free((ncl_set_response_item *)p);
}

static void ncl_item_dtor_sample(void *p)
{
    ncl_sample_item_free((ncl_sample_item *)p);
}

/* ------------------------------------------------------------- life cycle - */

ncl_message *ncl_message_new(ncl_msg_type type)
{
    ncl_message *msg = (ncl_message *)calloc(1, sizeof(ncl_message));
    if (msg == NULL) {
        return NULL;
    }
    msg->type = type;
    switch (type) {
    case NCL_MSG_QUERY_REQUEST:
        ncl_ptrvec_init(&msg->as.query_request.items, ncl_item_dtor_query_request);
        break;
    case NCL_MSG_QUERY_RESPONSE:
        ncl_ptrvec_init(&msg->as.query_response.items, ncl_item_dtor_query_response);
        break;
    case NCL_MSG_SET_REQUEST:
        ncl_ptrvec_init(&msg->as.set_request.items, ncl_item_dtor_set_request);
        break;
    case NCL_MSG_SET_RESPONSE:
        ncl_ptrvec_init(&msg->as.set_response.items, ncl_item_dtor_set_response);
        break;
    case NCL_MSG_SAMPLE:
        ncl_strvec_init(&msg->as.sample.paths);
        ncl_ptrvec_init(&msg->as.sample.data, ncl_item_dtor_sample);
        break;
    case NCL_MSG_METHOD_CALL_REQUEST:
        /* "check" is a non-null field, so it is serialised even when false. */
        msg->as.method_call_request.has_check = true;
        msg->as.method_call_request.check = false;
        break;
    default:
        break;
    }
    return msg;
}

void ncl_message_free(ncl_message *msg)
{
    if (msg == NULL) {
        return;
    }
    free(msg->message_id);

    switch (msg->type) {
    case NCL_MSG_PONG:
        free(msg->as.pong.open_api_schema);
        break;
    case NCL_MSG_PROBE_VERSION:
        free(msg->as.probe_version.version);
        break;
    case NCL_MSG_REGISTER_REQUEST:
        free(msg->as.register_request.device_id);
        break;
    case NCL_MSG_REGISTER_RESPONSE:
        free(msg->as.register_response.code);
        break;
    case NCL_MSG_QUERY_REQUEST:
        ncl_ptrvec_free(&msg->as.query_request.items);
        break;
    case NCL_MSG_QUERY_RESPONSE:
        ncl_ptrvec_free(&msg->as.query_response.items);
        break;
    case NCL_MSG_SET_REQUEST:
        ncl_ptrvec_free(&msg->as.set_request.items);
        break;
    case NCL_MSG_SET_RESPONSE:
        ncl_ptrvec_free(&msg->as.set_response.items);
        break;
    case NCL_MSG_PROBE_QUERY_RESPONSE:
        free(msg->as.probe_query_response.code);
        ncl_node_free(msg->as.probe_query_response.model);
        free(msg->as.probe_query_response.reason);
        break;
    case NCL_MSG_PROBE_SET_REQUEST:
        ncl_node_free(msg->as.probe_set_request.model);
        break;
    case NCL_MSG_PROBE_SET_RESPONSE:
        free(msg->as.probe_set_response.code);
        free(msg->as.probe_set_response.reason);
        break;
    case NCL_MSG_SAMPLE:
        ncl_strvec_free(&msg->as.sample.paths);
        free(msg->as.sample.id);
        free(msg->as.sample.begin_time);
        ncl_ptrvec_free(&msg->as.sample.data);
        break;
    case NCL_MSG_EVENT:
        free(msg->as.event.id);
        free(msg->as.event.time);
        ncl_json_free(msg->as.event.event);
        break;
    case NCL_MSG_METHOD_CALL_REQUEST:
        free(msg->as.method_call_request.method);
        ncl_json_free(msg->as.method_call_request.params);
        free(msg->as.method_call_request.token);
        break;
    case NCL_MSG_METHOD_CALL_RESPONSE:
        free(msg->as.method_call_response.code);
        free(msg->as.method_call_response.method);
        ncl_json_free(msg->as.method_call_response.params);
        free(msg->as.method_call_response.token);
        ncl_json_free(msg->as.method_call_response.data);
        free(msg->as.method_call_response.reason);
        break;
    default:
        break;
    }
    free(msg);
}

ncl_err ncl_message_set_message_id(ncl_message *msg, const char *id)
{
    char *copy;
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    copy = id != NULL ? ncl_strdup(id) : NULL;
    if (id != NULL && copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    free(msg->message_id);
    msg->message_id = copy;
    return NCL_OK;
}

static ncl_err ncl_msg_set_str(char **slot, const char *value)
{
    char *copy = value != NULL ? ncl_strdup(value) : NULL;
    if (value != NULL && copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    free(*slot);
    *slot = copy;
    return NCL_OK;
}

ncl_err ncl_message_finalise(ncl_message *msg)
{
    char uuid[37];
    char stamp[32];
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_message_is_valid(msg)) {
        return NCL_ERR_INVALID_MESSAGE;
    }
    /* The event time defaults to the current wall clock time. */
    if (msg->type == NCL_MSG_EVENT && ncl_str_is_empty(msg->as.event.time)) {
        snprintf(stamp, sizeof(stamp), "%lld", (long long)ncl_time_millis());
        if (ncl_msg_set_str(&msg->as.event.time, stamp) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }
    if (ncl_str_is_blank(msg->message_id)) {
        if (ncl_uuid4(uuid, sizeof(uuid)) != NCL_OK) {
            return NCL_ERR;
        }
        return ncl_message_set_message_id(msg, uuid);
    }
    return NCL_OK;
}

/* ----------------------------------------------------------- field setters- */

ncl_err ncl_message_set_code(ncl_message *msg, const char *code)
{
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_REGISTER_RESPONSE:
        return ncl_msg_set_str(&msg->as.register_response.code, code);
    case NCL_MSG_PROBE_QUERY_RESPONSE:
        return ncl_msg_set_str(&msg->as.probe_query_response.code, code);
    case NCL_MSG_PROBE_SET_RESPONSE:
        return ncl_msg_set_str(&msg->as.probe_set_response.code, code);
    case NCL_MSG_METHOD_CALL_RESPONSE:
        return ncl_msg_set_str(&msg->as.method_call_response.code, code);
    default:
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_err ncl_message_set_reason(ncl_message *msg, const char *reason)
{
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_PROBE_QUERY_RESPONSE:
        return ncl_msg_set_str(&msg->as.probe_query_response.reason, reason);
    case NCL_MSG_PROBE_SET_RESPONSE:
        return ncl_msg_set_str(&msg->as.probe_set_response.reason, reason);
    case NCL_MSG_METHOD_CALL_RESPONSE:
        return ncl_msg_set_str(&msg->as.method_call_response.reason, reason);
    default:
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_err ncl_message_set_open_api_schema(ncl_message *msg, const char *schema)
{
    if (msg == NULL || msg->type != NCL_MSG_PONG) {
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_msg_set_str(&msg->as.pong.open_api_schema, schema);
}

ncl_err ncl_message_set_version(ncl_message *msg, const char *version)
{
    if (msg == NULL || msg->type != NCL_MSG_PROBE_VERSION) {
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_msg_set_str(&msg->as.probe_version.version, version);
}

ncl_err ncl_message_set_device_id(ncl_message *msg, const char *device_id)
{
    if (msg == NULL || msg->type != NCL_MSG_REGISTER_REQUEST) {
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_msg_set_str(&msg->as.register_request.device_id, device_id);
}

ncl_err ncl_message_set_model(ncl_message *msg, ncl_node *model)
{
    if (msg == NULL) {
        ncl_node_free(model);
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_PROBE_QUERY_RESPONSE:
        ncl_node_free(msg->as.probe_query_response.model);
        msg->as.probe_query_response.model = model;
        return NCL_OK;
    case NCL_MSG_PROBE_SET_REQUEST:
        ncl_node_free(msg->as.probe_set_request.model);
        msg->as.probe_set_request.model = model;
        return NCL_OK;
    default:
        ncl_node_free(model);
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_node *ncl_message_take_model(ncl_message *msg)
{
    ncl_node *model = NULL;

    if (msg == NULL) {
        return NULL;
    }
    switch (msg->type) {
    case NCL_MSG_PROBE_QUERY_RESPONSE:
        model = msg->as.probe_query_response.model;
        msg->as.probe_query_response.model = NULL;
        break;
    case NCL_MSG_PROBE_SET_REQUEST:
        model = msg->as.probe_set_request.model;
        msg->as.probe_set_request.model = NULL;
        break;
    default:
        break;
    }
    return model;
}

ncl_err ncl_message_set_method(ncl_message *msg, const char *method)
{
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_METHOD_CALL_REQUEST:
        return ncl_msg_set_str(&msg->as.method_call_request.method, method);
    case NCL_MSG_METHOD_CALL_RESPONSE:
        return ncl_msg_set_str(&msg->as.method_call_response.method, method);
    default:
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_err ncl_message_set_params(ncl_message *msg, ncl_json *params)
{
    if (msg == NULL) {
        ncl_json_free(params);
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_METHOD_CALL_REQUEST:
        ncl_json_free(msg->as.method_call_request.params);
        msg->as.method_call_request.params = params;
        return NCL_OK;
    case NCL_MSG_METHOD_CALL_RESPONSE:
        ncl_json_free(msg->as.method_call_response.params);
        msg->as.method_call_response.params = params;
        return NCL_OK;
    default:
        ncl_json_free(params);
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_err ncl_message_set_token(ncl_message *msg, const char *token)
{
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_METHOD_CALL_REQUEST:
        return ncl_msg_set_str(&msg->as.method_call_request.token, token);
    case NCL_MSG_METHOD_CALL_RESPONSE:
        return ncl_msg_set_str(&msg->as.method_call_response.token, token);
    default:
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_err ncl_message_set_check(ncl_message *msg, bool check)
{
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (msg->type) {
    case NCL_MSG_METHOD_CALL_REQUEST:
        msg->as.method_call_request.check = check;
        msg->as.method_call_request.has_check = true;
        return NCL_OK;
    case NCL_MSG_METHOD_CALL_RESPONSE:
        msg->as.method_call_response.check = check;
        msg->as.method_call_response.has_check = true;
        return NCL_OK;
    default:
        return NCL_ERR_INVALID_TYPE;
    }
}

ncl_err ncl_message_set_data(ncl_message *msg, ncl_json *data)
{
    if (msg == NULL || msg->type != NCL_MSG_METHOD_CALL_RESPONSE) {
        ncl_json_free(data);
        return NCL_ERR_INVALID_TYPE;
    }
    ncl_json_free(msg->as.method_call_response.data);
    msg->as.method_call_response.data = data;
    return NCL_OK;
}

ncl_err ncl_message_set_event(ncl_message *msg, ncl_json *event)
{
    if (msg == NULL || msg->type != NCL_MSG_EVENT) {
        ncl_json_free(event);
        return NCL_ERR_INVALID_TYPE;
    }
    ncl_json_free(msg->as.event.event);
    msg->as.event.event = event;
    return NCL_OK;
}

ncl_err ncl_message_set_sample_id(ncl_message *msg, const char *id)
{
    if (msg == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (msg->type == NCL_MSG_EVENT) {
        return ncl_msg_set_str(&msg->as.event.id, id);
    }
    if (msg->type != NCL_MSG_SAMPLE) {
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_msg_set_str(&msg->as.sample.id, id);
}

ncl_err ncl_message_set_event_time_ms(ncl_message *msg, int64_t millis)
{
    char stamp[32];

    if (msg == NULL || msg->type != NCL_MSG_EVENT) {
        return NCL_ERR_INVALID_TYPE;
    }
    snprintf(stamp, sizeof(stamp), "%lld",
             (long long)(millis > 0 ? millis : ncl_time_millis()));
    return ncl_msg_set_str(&msg->as.event.time, stamp);
}

ncl_err ncl_message_set_begin_time(ncl_message *msg, const char *begin_time)
{
    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_msg_set_str(&msg->as.sample.begin_time, begin_time);
}

ncl_err ncl_message_set_sample_interval(ncl_message *msg, long long interval)
{
    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        return NCL_ERR_INVALID_TYPE;
    }
    msg->as.sample.interval = interval;
    msg->as.sample.has_interval = true;
    return NCL_OK;
}

ncl_err ncl_message_set_upload_interval(ncl_message *msg, long long interval)
{
    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        return NCL_ERR_INVALID_TYPE;
    }
    msg->as.sample.upload_interval = interval;
    msg->as.sample.has_upload_interval = true;
    return NCL_OK;
}

ncl_err ncl_message_add_sample_path(ncl_message *msg, const char *path)
{
    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_strvec_push(&msg->as.sample.paths, path);
}

ncl_err ncl_message_add_sample_item(ncl_message *msg, ncl_sample_item *item)
{
    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        ncl_sample_item_free(item);
        return NCL_ERR_INVALID_TYPE;
    }
    if (!ncl_sample_item_is_valid(item)) {
        ncl_sample_item_free(item);
        return NCL_ERR_INVALID_ITEM;
    }
    return ncl_ptrvec_push_owned(&msg->as.sample.data, item);
}

/* ---------------------------------------------------------- item addition - */

ncl_err ncl_message_add_query_request_item(ncl_message *msg,
                                           ncl_query_request_item *item)
{
    if (msg == NULL || msg->type != NCL_MSG_QUERY_REQUEST) {
        ncl_query_request_item_free(item);
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_ptrvec_push_owned(&msg->as.query_request.items, item);
}

ncl_err ncl_message_add_query_response_item(ncl_message *msg,
                                            ncl_query_response_item *item)
{
    if (msg == NULL || msg->type != NCL_MSG_QUERY_RESPONSE) {
        ncl_query_response_item_free(item);
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_ptrvec_push_owned(&msg->as.query_response.items, item);
}

ncl_err ncl_message_add_set_request_item(ncl_message *msg, ncl_set_request_item *item)
{
    if (msg == NULL || msg->type != NCL_MSG_SET_REQUEST) {
        ncl_set_request_item_free(item);
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_ptrvec_push_owned(&msg->as.set_request.items, item);
}

ncl_err ncl_message_add_set_response_item(ncl_message *msg,
                                          ncl_set_response_item *item)
{
    if (msg == NULL || msg->type != NCL_MSG_SET_RESPONSE) {
        ncl_set_response_item_free(item);
        return NCL_ERR_INVALID_TYPE;
    }
    return ncl_ptrvec_push_owned(&msg->as.set_response.items, item);
}

size_t ncl_message_item_count(const ncl_message *msg)
{
    if (msg == NULL) {
        return 0;
    }
    switch (msg->type) {
    case NCL_MSG_QUERY_REQUEST: return ncl_ptrvec_len(&msg->as.query_request.items);
    case NCL_MSG_QUERY_RESPONSE: return ncl_ptrvec_len(&msg->as.query_response.items);
    case NCL_MSG_SET_REQUEST: return ncl_ptrvec_len(&msg->as.set_request.items);
    case NCL_MSG_SET_RESPONSE: return ncl_ptrvec_len(&msg->as.set_response.items);
    case NCL_MSG_SAMPLE: return ncl_ptrvec_len(&msg->as.sample.data);
    default: return 0;
    }
}

void *ncl_message_item_at(const ncl_message *msg, size_t index)
{
    if (msg == NULL) {
        return NULL;
    }
    switch (msg->type) {
    case NCL_MSG_QUERY_REQUEST:
        return ncl_ptrvec_at(&msg->as.query_request.items, index);
    case NCL_MSG_QUERY_RESPONSE:
        return ncl_ptrvec_at(&msg->as.query_response.items, index);
    case NCL_MSG_SET_REQUEST:
        return ncl_ptrvec_at(&msg->as.set_request.items, index);
    case NCL_MSG_SET_RESPONSE:
        return ncl_ptrvec_at(&msg->as.set_response.items, index);
    case NCL_MSG_SAMPLE:
        return ncl_ptrvec_at(&msg->as.sample.data, index);
    default:
        return NULL;
    }
}

/* ============================================================= validation = */

bool ncl_message_is_valid(const ncl_message *msg)
{
    size_t i;
    if (msg == NULL) {
        return false;
    }
    switch (msg->type) {
    case NCL_MSG_PING:
        /* A Ping is valid once it carries "@id". */
        return !ncl_str_is_blank(msg->message_id);

    case NCL_MSG_PONG:
        return true;

    case NCL_MSG_PROBE_VERSION:
        return !ncl_str_is_empty(msg->as.probe_version.version);

    case NCL_MSG_REGISTER_REQUEST:
        return !ncl_str_is_empty(msg->as.register_request.device_id);

    case NCL_MSG_REGISTER_RESPONSE:
        return ncl_check_is_code_valid(msg->as.register_response.code);

    case NCL_MSG_QUERY_REQUEST:
        if (ncl_ptrvec_len(&msg->as.query_request.items) == 0) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&msg->as.query_request.items); i++) {
            if (!ncl_query_request_item_is_valid(
                    (const ncl_query_request_item *)ncl_ptrvec_at(
                        &msg->as.query_request.items, i))) {
                return false;
            }
        }
        return true;

    case NCL_MSG_QUERY_RESPONSE:
        if (ncl_ptrvec_len(&msg->as.query_response.items) == 0) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&msg->as.query_response.items); i++) {
            if (!ncl_query_response_item_is_valid(
                    (const ncl_query_response_item *)ncl_ptrvec_at(
                        &msg->as.query_response.items, i))) {
                return false;
            }
        }
        return true;

    case NCL_MSG_SET_REQUEST:
        if (ncl_ptrvec_len(&msg->as.set_request.items) == 0) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&msg->as.set_request.items); i++) {
            if (!ncl_set_request_item_is_valid(
                    (const ncl_set_request_item *)ncl_ptrvec_at(
                        &msg->as.set_request.items, i))) {
                return false;
            }
        }
        return true;

    case NCL_MSG_SET_RESPONSE:
        if (ncl_ptrvec_len(&msg->as.set_response.items) == 0) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&msg->as.set_response.items); i++) {
            if (!ncl_set_response_item_is_valid(
                    (const ncl_set_response_item *)ncl_ptrvec_at(
                        &msg->as.set_response.items, i))) {
                return false;
            }
        }
        return true;

    case NCL_MSG_PROBE_QUERY_REQUEST:
        return true;

    case NCL_MSG_PROBE_QUERY_RESPONSE:
        if (ncl_check_is_code_ok(msg->as.probe_query_response.code)) {
            return msg->as.probe_query_response.model != NULL &&
                   ncl_node_is_valid(msg->as.probe_query_response.model);
        }
        if (ncl_check_is_code_ng(msg->as.probe_query_response.code)) {
            return msg->as.probe_query_response.model == NULL;
        }
        return false;

    case NCL_MSG_PROBE_SET_REQUEST:
        return msg->as.probe_set_request.model != NULL &&
               ncl_node_is_valid(msg->as.probe_set_request.model);

    case NCL_MSG_PROBE_SET_RESPONSE:
        return ncl_check_is_code_valid(msg->as.probe_set_response.code);

    case NCL_MSG_SAMPLE: {
        size_t expected;
        if (ncl_ptrvec_len(&msg->as.sample.data) == 0) {
            return false;
        }
        {
            const ncl_sample_item *first =
                (const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, 0);
            if (first == NULL) {
                return false;
            }
            expected = ncl_json_arr_len(first->data);
        }
        for (i = 1; i < ncl_ptrvec_len(&msg->as.sample.data); i++) {
            const ncl_sample_item *item =
                (const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, i);
            if (item == NULL || ncl_json_arr_len(item->data) != expected) {
                return false;
            }
        }
        return true;
    }

    case NCL_MSG_EVENT:
        return msg->as.event.event != NULL && msg->as.event.id != NULL;

    case NCL_MSG_METHOD_CALL_REQUEST:
        return !ncl_str_is_blank(msg->as.method_call_request.method);

    case NCL_MSG_METHOD_CALL_RESPONSE:
        return !ncl_str_is_blank(msg->as.method_call_response.method) &&
               msg->as.method_call_response.code != NULL;

    default:
        return false;
    }
}

bool ncl_message_matches(const ncl_message *response, const ncl_message *request)
{
    size_t i;
    if (response == NULL || request == NULL) {
        return false;
    }
    if (response->type == NCL_MSG_QUERY_RESPONSE &&
        request->type == NCL_MSG_QUERY_REQUEST) {
        if (response->message_id != NULL) {
            return request->message_id != NULL &&
                   strcmp(response->message_id, request->message_id) == 0;
        }
        if (!ncl_message_is_valid(request) || !ncl_message_is_valid(response)) {
            return false;
        }
        if (ncl_ptrvec_len(&request->as.query_request.items) !=
            ncl_ptrvec_len(&response->as.query_response.items)) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&request->as.query_request.items); i++) {
            const ncl_query_response_item *item =
                (const ncl_query_response_item *)ncl_ptrvec_at(
                    &response->as.query_response.items, i);
            const ncl_query_request_item *req =
                (const ncl_query_request_item *)ncl_ptrvec_at(
                    &request->as.query_request.items, i);
            if (!ncl_query_response_item_matches(item, req)) {
                return false;
            }
        }
        return true;
    }

    if (response->type == NCL_MSG_SET_RESPONSE &&
        request->type == NCL_MSG_SET_REQUEST) {
        if (response->message_id != NULL) {
            return request->message_id != NULL &&
                   strcmp(response->message_id, request->message_id) == 0;
        }
        if (!ncl_message_is_valid(request) || !ncl_message_is_valid(response)) {
            return false;
        }
        if (ncl_ptrvec_len(&request->as.set_request.items) !=
            ncl_ptrvec_len(&response->as.set_response.items)) {
            return false;
        }
        for (i = 0; i < ncl_ptrvec_len(&request->as.set_request.items); i++) {
            const ncl_set_response_item *item =
                (const ncl_set_response_item *)ncl_ptrvec_at(
                    &response->as.set_response.items, i);
            const ncl_set_request_item *req =
                (const ncl_set_request_item *)ncl_ptrvec_at(
                    &request->as.set_request.items, i);
            if (!ncl_set_response_item_matches(item, req)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool ncl_message_has_data(const ncl_message *msg)
{
    size_t i;
    if (msg == NULL || msg->type != NCL_MSG_QUERY_RESPONSE) {
        return false;
    }
    for (i = 0; i < ncl_ptrvec_len(&msg->as.query_response.items); i++) {
        if (ncl_query_response_item_has_data(
                (const ncl_query_response_item *)ncl_ptrvec_at(
                    &msg->as.query_response.items, i))) {
            return true;
        }
    }
    return false;
}

ncl_json *ncl_message_get_data(const ncl_message *msg)
{
    size_t i;
    if (msg == NULL || msg->type != NCL_MSG_QUERY_RESPONSE) {
        return NULL;
    }
    for (i = 0; i < ncl_ptrvec_len(&msg->as.query_response.items); i++) {
        const ncl_query_response_item *item =
            (const ncl_query_response_item *)ncl_ptrvec_at(
                &msg->as.query_response.items, i);
        if (ncl_query_response_item_has_data(item)) {
            return ncl_query_response_item_data(item);
        }
    }
    return NULL;
}

char *ncl_message_sample_header(const ncl_message *msg, const char *separator)
{
    ncl_strbuf sb;
    size_t i;
    const char *sep = separator != NULL ? separator : ";";

    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        return NULL;
    }
    ncl_strbuf_init(&sb);
    for (i = 0; i < ncl_strvec_len(&msg->as.sample.paths); i++) {
        if (i > 0) {
            ncl_strbuf_puts(&sb, sep);
        }
        ncl_strbuf_puts(&sb, ncl_strvec_at(&msg->as.sample.paths, i));
    }
    return ncl_strbuf_detach(&sb);
}

bool ncl_message_sample_is_complete(const ncl_message *msg)
{
    size_t i;
    size_t slots;

    if (msg == NULL || msg->type != NCL_MSG_SAMPLE) {
        return false;
    }
    /* 表头必须在，且与数据块列数一致（消费端按下标对应两者）。 */
    if (ncl_strvec_len(&msg->as.sample.paths) == 0 ||
        ncl_strvec_len(&msg->as.sample.paths) !=
            ncl_ptrvec_len(&msg->as.sample.data)) {
        return false;
    }
    for (i = 0; i < ncl_ptrvec_len(&msg->as.sample.data); i++) {
        const ncl_sample_item *item =
            (const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, i);
        if (item == NULL || item->data == NULL) {
            return false;
        }
    }
    /* 各列槽位数一致（外层对齐规则）。 */
    if (!ncl_message_is_valid(msg)) {
        return false;
    }

    /*
     * 亚毫秒采样时，一个槽位的值可能是一批数据（数组）。消费端按行列对读，
     * 因此**内层与外层都要对齐**，一条报文只允许一种统一形状：
     *
     *   标量形状：每个槽位、每一列都是标量/null
     *   批量形状：每个槽位、每一列都是数组，且**所有内层长度完全相同**
     *
     * 形状由第一列第一个槽位决定，其余位置必须与之完全一致；任何一个槽位
     * 或列不符合（标量混批量、或批长不一致）都判为不完整。
     */
    slots = ncl_json_arr_len(
        ((const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, 0))->data);
    {
        const ncl_json *reference = ncl_json_arr_get(
            ((const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, 0))->data,
            0);
        bool batch_shape =
            reference != NULL && ncl_json_type_of(reference) == NCL_JSON_ARRAY;
        size_t batch_len = batch_shape ? ncl_json_arr_len(reference) : 0;
        size_t column;

        for (column = 0; column < ncl_ptrvec_len(&msg->as.sample.data); column++) {
            const ncl_sample_item *item =
                (const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data,
                                                       column);
            for (i = 0; i < slots; i++) {
                const ncl_json *value = ncl_json_arr_get(item->data, i);
                bool is_array =
                    value != NULL && ncl_json_type_of(value) == NCL_JSON_ARRAY;

                if (is_array != batch_shape) {
                    return false; /* 标量与批量混在一张表里，没法对读 */
                }
                if (batch_shape && ncl_json_arr_len(value) != batch_len) {
                    return false; /* 内层长度不一致 */
                }
            }
        }
    }
    return true;
}

bool ncl_sample_item_is_nested(const ncl_sample_item *item)
{
    size_t i;

    if (item == NULL || item->data == NULL) {
        return false;
    }
    for (i = 0; i < ncl_json_arr_len(item->data); i++) {
        const ncl_json *value = ncl_json_arr_get(item->data, i);
        if (value != NULL && ncl_json_type_of(value) == NCL_JSON_ARRAY) {
            return true;
        }
    }
    return false;
}

size_t ncl_sample_item_value_count(const ncl_sample_item *item)
{
    size_t i;
    size_t total = 0;

    if (item == NULL || item->data == NULL) {
        return 0;
    }
    for (i = 0; i < ncl_json_arr_len(item->data); i++) {
        const ncl_json *value = ncl_json_arr_get(item->data, i);
        total += (value != NULL && ncl_json_type_of(value) == NCL_JSON_ARRAY)
                     ? ncl_json_arr_len(value)
                     : 1;
    }
    return total;
}

const ncl_json *ncl_sample_item_value_at(const ncl_sample_item *item,
                                         size_t index)
{
    size_t i;
    size_t seen = 0;

    if (item == NULL || item->data == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_json_arr_len(item->data); i++) {
        const ncl_json *value = ncl_json_arr_get(item->data, i);
        if (value != NULL && ncl_json_type_of(value) == NCL_JSON_ARRAY) {
            size_t length = ncl_json_arr_len(value);
            if (index < seen + length) {
                return ncl_json_arr_get(value, index - seen);
            }
            seen += length;
        } else {
            if (index == seen) {
                return value;
            }
            seen += 1;
        }
    }
    return NULL;
}

size_t ncl_message_sample_point_count(const ncl_message *msg)
{
    const ncl_sample_item *first;

    if (msg == NULL || msg->type != NCL_MSG_SAMPLE ||
        ncl_ptrvec_len(&msg->as.sample.data) == 0) {
        return 0;
    }
    first = (const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, 0);
    return ncl_sample_item_value_count(first);
}

/* ========================================================== serialisation = */

static void ncl_json_set_if(ncl_json *obj, const char *key, const char *value)
{
    if (value != NULL) {
        ncl_json_obj_set_string(obj, key, value);
    }
}

static void ncl_json_set_id(ncl_json *obj, const char *message_id)
{
    ncl_json_set_if(obj, "@id", message_id);
}

ncl_json *ncl_message_to_json(const ncl_message *msg)
{
    ncl_json *obj;
    size_t i;

    if (msg == NULL) {
        return ncl_json_new_null();
    }
    obj = ncl_json_new_object();
    if (obj == NULL) {
        return NULL;
    }

    /* Query and set response items have no "@id" of their own, but those are
     * items, not messages, so every message emits it first. */
    ncl_json_set_id(obj, msg->message_id);

    switch (msg->type) {
    case NCL_MSG_PING:
        break;

    case NCL_MSG_PONG:
        ncl_json_set_if(obj, "OpenApiSchema", msg->as.pong.open_api_schema);
        break;

    case NCL_MSG_PROBE_VERSION:
        ncl_json_set_if(obj, "version", msg->as.probe_version.version);
        break;

    case NCL_MSG_REGISTER_REQUEST:
        ncl_json_set_if(obj, "deviceid", msg->as.register_request.device_id);
        break;

    case NCL_MSG_REGISTER_RESPONSE:
        ncl_json_set_if(obj, "code", msg->as.register_response.code);
        break;

    case NCL_MSG_QUERY_REQUEST: {
        ncl_json *arr = ncl_json_new_array();
        for (i = 0; arr != NULL && i < ncl_ptrvec_len(&msg->as.query_request.items); i++) {
            const ncl_query_request_item *item =
                (const ncl_query_request_item *)ncl_ptrvec_at(
                    &msg->as.query_request.items, i);
            ncl_json *entry = ncl_json_new_object();
            if (entry == NULL) {
                break;
            }
            ncl_json_set_if(entry, "id", item->id);
            if (item->params != NULL) {
                ncl_json_obj_set(entry, "params", ncl_json_clone(item->params));
            }
            if (ncl_json_arr_push(arr, entry) != NCL_OK) {
                break;
            }
        }
        if (ncl_ptrvec_len(&msg->as.query_request.items) > 0) {
            ncl_json_obj_set(obj, "ids", arr);
        } else {
            ncl_json_free(arr);
        }
        break;
    }

    case NCL_MSG_QUERY_RESPONSE: {
        ncl_json *arr = ncl_json_new_array();
        for (i = 0; arr != NULL && i < ncl_ptrvec_len(&msg->as.query_response.items); i++) {
            const ncl_query_response_item *item =
                (const ncl_query_response_item *)ncl_ptrvec_at(
                    &msg->as.query_response.items, i);
            ncl_json *entry = ncl_json_new_object();
            if (entry == NULL) {
                break;
            }
            ncl_json_set_if(entry, "id", item->id);
            ncl_json_set_if(entry, "code", item->code);
            ncl_json_set_if(entry, "reason", item->reason);
            if (item->params != NULL) {
                ncl_json_obj_set(entry, "params", ncl_json_clone(item->params));
            }
            if (ncl_json_arr_len(item->values) > 0) {
                ncl_json_obj_set(entry, "values", ncl_json_clone(item->values));
            }
            if (ncl_json_arr_push(arr, entry) != NCL_OK) {
                break;
            }
        }
        if (ncl_ptrvec_len(&msg->as.query_response.items) > 0) {
            ncl_json_obj_set(obj, "values", arr);
        } else {
            ncl_json_free(arr);
        }
        break;
    }

    case NCL_MSG_SET_REQUEST: {
        ncl_json *arr = ncl_json_new_array();
        for (i = 0; arr != NULL && i < ncl_ptrvec_len(&msg->as.set_request.items); i++) {
            const ncl_set_request_item *item =
                (const ncl_set_request_item *)ncl_ptrvec_at(
                    &msg->as.set_request.items, i);
            ncl_json *entry = ncl_json_new_object();
            if (entry == NULL) {
                break;
            }
            ncl_json_set_if(entry, "id", item->id);
            if (item->params != NULL) {
                ncl_json_obj_set(entry, "params", ncl_json_clone(item->params));
            }
            if (ncl_json_arr_push(arr, entry) != NCL_OK) {
                break;
            }
        }
        if (ncl_ptrvec_len(&msg->as.set_request.items) > 0) {
            ncl_json_obj_set(obj, "values", arr);
        } else {
            ncl_json_free(arr);
        }
        break;
    }

    case NCL_MSG_SET_RESPONSE: {
        ncl_json *arr = ncl_json_new_array();
        for (i = 0; arr != NULL && i < ncl_ptrvec_len(&msg->as.set_response.items); i++) {
            const ncl_set_response_item *item =
                (const ncl_set_response_item *)ncl_ptrvec_at(
                    &msg->as.set_response.items, i);
            ncl_json *entry = ncl_json_new_object();
            if (entry == NULL) {
                break;
            }
            ncl_json_set_if(entry, "id", item->id);
            ncl_json_set_if(entry, "code", item->code);
            ncl_json_set_if(entry, "reason", item->reason);
            if (item->params != NULL) {
                ncl_json_obj_set(entry, "params", ncl_json_clone(item->params));
            }
            if (item->result != NULL) {
                ncl_json_obj_set(entry, "result", ncl_json_clone(item->result));
            }
            if (ncl_json_arr_push(arr, entry) != NCL_OK) {
                break;
            }
        }
        if (ncl_ptrvec_len(&msg->as.set_response.items) > 0) {
            ncl_json_obj_set(obj, "results", arr);
        } else {
            ncl_json_free(arr);
        }
        break;
    }

    case NCL_MSG_PROBE_QUERY_REQUEST:
        break;

    case NCL_MSG_PROBE_QUERY_RESPONSE:
        ncl_json_set_if(obj, "code", msg->as.probe_query_response.code);
        if (msg->as.probe_query_response.model != NULL) {
            ncl_json_obj_set(obj, "probe",
                             ncl_node_to_json(msg->as.probe_query_response.model));
        }
        ncl_json_set_if(obj, "reason", msg->as.probe_query_response.reason);
        break;

    case NCL_MSG_PROBE_SET_REQUEST:
        if (msg->as.probe_set_request.model != NULL) {
            ncl_json_obj_set(obj, "probe",
                             ncl_node_to_json(msg->as.probe_set_request.model));
        }
        break;

    case NCL_MSG_PROBE_SET_RESPONSE:
        ncl_json_set_if(obj, "code", msg->as.probe_set_response.code);
        ncl_json_set_if(obj, "reason", msg->as.probe_set_response.reason);
        break;

    case NCL_MSG_SAMPLE: {
        ncl_json *arr;
        if (ncl_strvec_len(&msg->as.sample.paths) > 0) {
            ncl_json_obj_set(obj, "paths", ncl_strvec_to_json(&msg->as.sample.paths));
        }
        ncl_json_set_if(obj, "id", msg->as.sample.id);
        ncl_json_set_if(obj, "beginTime", msg->as.sample.begin_time);
        arr = ncl_json_new_array();
        for (i = 0; arr != NULL && i < ncl_ptrvec_len(&msg->as.sample.data); i++) {
            const ncl_sample_item *item =
                (const ncl_sample_item *)ncl_ptrvec_at(&msg->as.sample.data, i);
            ncl_json *entry = ncl_json_new_object();
            if (entry == NULL) {
                break;
            }
            ncl_json_set_if(entry, "encoding", item->encoding);
            if (item->data != NULL) {
                ncl_json_obj_set(entry, "data", ncl_json_clone(item->data));
            }
            if (ncl_json_arr_push(arr, entry) != NCL_OK) {
                break;
            }
        }
        if (ncl_ptrvec_len(&msg->as.sample.data) > 0) {
            ncl_json_obj_set(obj, "data", arr);
        } else {
            ncl_json_free(arr);
        }
        if (msg->as.sample.has_interval) {
            ncl_json_obj_set_int(obj, "interval", msg->as.sample.interval);
        }
        if (msg->as.sample.has_upload_interval) {
            ncl_json_obj_set_int(obj, "uploadInterval", msg->as.sample.upload_interval);
        }
        break;
    }

    case NCL_MSG_EVENT:
        ncl_json_set_if(obj, "id", msg->as.event.id);
        ncl_json_set_if(obj, "time", msg->as.event.time);
        if (msg->as.event.event != NULL) {
            ncl_json_obj_set(obj, "event", ncl_json_clone(msg->as.event.event));
        }
        break;

    case NCL_MSG_METHOD_CALL_REQUEST:
        ncl_json_set_if(obj, "method", msg->as.method_call_request.method);
        if (msg->as.method_call_request.params != NULL) {
            ncl_json_obj_set(obj, "params",
                             ncl_json_clone(msg->as.method_call_request.params));
        }
        if (msg->as.method_call_request.has_check) {
            ncl_json_obj_set_bool(obj, "check", msg->as.method_call_request.check);
        }
        ncl_json_set_if(obj, "token", msg->as.method_call_request.token);
        break;

    case NCL_MSG_METHOD_CALL_RESPONSE:
        ncl_json_set_if(obj, "code", msg->as.method_call_response.code);
        ncl_json_set_if(obj, "method", msg->as.method_call_response.method);
        if (msg->as.method_call_response.params != NULL) {
            ncl_json_obj_set(obj, "params",
                             ncl_json_clone(msg->as.method_call_response.params));
        }
        if (msg->as.method_call_response.has_check) {
            ncl_json_obj_set_bool(obj, "check", msg->as.method_call_response.check);
        }
        ncl_json_set_if(obj, "token", msg->as.method_call_response.token);
        if (msg->as.method_call_response.data != NULL) {
            ncl_json_obj_set(obj, "data",
                             ncl_json_clone(msg->as.method_call_response.data));
        }
        ncl_json_set_if(obj, "reason", msg->as.method_call_response.reason);
        break;

    default:
        break;
    }
    return obj;
}

char *ncl_message_write_string(const ncl_message *msg)
{
    ncl_json *json = ncl_message_to_json(msg);
    char *text;
    if (json == NULL) {
        return NULL;
    }
    text = ncl_json_write_string(json);
    ncl_json_free(json);
    return text;
}

/* ======================================================== deserialisation = */

static char *ncl_msg_read_string(const ncl_json *obj, const char *key)
{
    const char *s = ncl_json_obj_get_string(obj, key);
    return s != NULL ? ncl_strdup(s) : NULL;
}

/* Sample.beginTime and Event.time are strings on the wire. */
static char *ncl_msg_read_number_as_string(const ncl_json *obj, const char *key)
{
    return ncl_json_as_text(ncl_json_obj_get(obj, key));
}

static void ncl_item_read_params_into(const ncl_json *entry, ncl_json **params)
{
    ncl_json *value = ncl_json_obj_get(entry, "params");
    if (value != NULL) {
        *params = ncl_json_clone(value);
    }
}

static ncl_message *ncl_query_request_from_json(const ncl_json *json)
{
    ncl_message *msg = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_json *ids = ncl_json_obj_get(json, "ids");
    size_t i;
    if (msg == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_json_arr_len(ids); i++) {
        const ncl_json *entry = ncl_json_arr_get(ids, i);
        ncl_query_request_item *item =
            ncl_query_request_item_new(ncl_json_obj_get_string(entry, "id"));
        if (item == NULL) {
            ncl_message_free(msg);
            return NULL;
        }
        ncl_item_read_params_into(entry, &item->params);
        if (ncl_message_add_query_request_item(msg, item) != NCL_OK) {
            ncl_message_free(msg);
            return NULL;
        }
    }
    return msg;
}

static ncl_message *ncl_query_response_from_json(const ncl_json *json)
{
    ncl_message *msg = ncl_message_new(NCL_MSG_QUERY_RESPONSE);
    ncl_json *values = ncl_json_obj_get(json, "values");
    size_t i;
    if (msg == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_json_arr_len(values); i++) {
        const ncl_json *entry = ncl_json_arr_get(values, i);
        ncl_query_response_item *item =
            ncl_query_response_item_new(ncl_json_obj_get_string(entry, "id"));
        ncl_json *entry_values;
        if (item == NULL) {
            ncl_message_free(msg);
            return NULL;
        }
        item->code = ncl_msg_read_string(entry, "code");
        item->reason = ncl_msg_read_string(entry, "reason");
        ncl_item_read_params_into(entry, &item->params);
        entry_values = ncl_json_obj_get(entry, "values");
        if (entry_values != NULL) {
            ncl_json_free(item->values);
            item->values = ncl_json_clone(entry_values);
        }
        if (ncl_message_add_query_response_item(msg, item) != NCL_OK) {
            ncl_message_free(msg);
            return NULL;
        }
    }
    return msg;
}

static ncl_message *ncl_set_request_from_json(const ncl_json *json)
{
    ncl_message *msg = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_json *values = ncl_json_obj_get(json, "values");
    size_t i;
    if (msg == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_json_arr_len(values); i++) {
        const ncl_json *entry = ncl_json_arr_get(values, i);
        ncl_set_request_item *item =
            ncl_set_request_item_new(ncl_json_obj_get_string(entry, "id"));
        if (item == NULL) {
            ncl_message_free(msg);
            return NULL;
        }
        ncl_item_read_params_into(entry, &item->params);
        if (ncl_message_add_set_request_item(msg, item) != NCL_OK) {
            ncl_message_free(msg);
            return NULL;
        }
    }
    return msg;
}

static ncl_message *ncl_set_response_from_json(const ncl_json *json)
{
    ncl_message *msg = ncl_message_new(NCL_MSG_SET_RESPONSE);
    ncl_json *results = ncl_json_obj_get(json, "results");
    size_t i;
    if (msg == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_json_arr_len(results); i++) {
        const ncl_json *entry = ncl_json_arr_get(results, i);
        ncl_set_response_item *item =
            ncl_set_response_item_new(ncl_json_obj_get_string(entry, "id"));
        ncl_json *result;
        if (item == NULL) {
            ncl_message_free(msg);
            return NULL;
        }
        item->code = ncl_msg_read_string(entry, "code");
        item->reason = ncl_msg_read_string(entry, "reason");
        ncl_item_read_params_into(entry, &item->params);
        result = ncl_json_obj_get(entry, "result");
        if (result != NULL) {
            item->result = ncl_json_clone(result);
        }
        if (ncl_message_add_set_response_item(msg, item) != NCL_OK) {
            ncl_message_free(msg);
            return NULL;
        }
    }
    return msg;
}

static ncl_message *ncl_sample_from_json(const ncl_json *json)
{
    ncl_message *msg = ncl_message_new(NCL_MSG_SAMPLE);
    ncl_json *paths;
    ncl_json *data;
    size_t i;

    if (msg == NULL) {
        return NULL;
    }
    paths = ncl_json_obj_get(json, "paths");
    for (i = 0; i < ncl_json_arr_len(paths); i++) {
        const char *path = ncl_json_as_string(ncl_json_arr_get(paths, i));
        if (path != NULL) {
            ncl_message_add_sample_path(msg, path);
        }
    }
    msg->as.sample.id = ncl_msg_read_string(json, "id");
    msg->as.sample.begin_time = ncl_msg_read_number_as_string(json, "beginTime");

    data = ncl_json_obj_get(json, "data");
    for (i = 0; i < ncl_json_arr_len(data); i++) {
        const ncl_json *entry = ncl_json_arr_get(data, i);
        ncl_sample_item *item = ncl_sample_item_new();
        if (item == NULL) {
            ncl_message_free(msg);
            return NULL;
        }
        item->encoding = ncl_msg_read_string(entry, "encoding");
        {
            ncl_json *values = ncl_json_obj_get(entry, "data");
            if (values != NULL) {
                ncl_json_free(item->data);
                item->data = ncl_json_clone(values);
                if (item->data == NULL) {
                    ncl_sample_item_free(item);
                    ncl_message_free(msg);
                    return NULL;
                }
            }
        }
        if (ncl_message_add_sample_item(msg, item) != NCL_OK) {
            ncl_message_free(msg);
            return NULL;
        }
    }
    if (ncl_json_obj_has(json, "interval")) {
        msg->as.sample.has_interval = ncl_json_as_int(
            ncl_json_obj_get(json, "interval"), &msg->as.sample.interval);
    }
    if (ncl_json_obj_has(json, "uploadInterval")) {
        msg->as.sample.has_upload_interval = ncl_json_as_int(
            ncl_json_obj_get(json, "uploadInterval"), &msg->as.sample.upload_interval);
    }
    return msg;
}

ncl_message *ncl_message_from_json(ncl_msg_type type, const ncl_json *json)
{
    ncl_message *msg;

    if (json == NULL || ncl_json_type_of(json) != NCL_JSON_OBJECT) {
        return NULL;
    }

    /* The list-bearing kinds have dedicated readers; the rest are filled in
     * below. "@id" is assigned once, for every kind. */
    switch (type) {
    case NCL_MSG_QUERY_REQUEST: msg = ncl_query_request_from_json(json); break;
    case NCL_MSG_QUERY_RESPONSE: msg = ncl_query_response_from_json(json); break;
    case NCL_MSG_SET_REQUEST: msg = ncl_set_request_from_json(json); break;
    case NCL_MSG_SET_RESPONSE: msg = ncl_set_response_from_json(json); break;
    case NCL_MSG_SAMPLE: msg = ncl_sample_from_json(json); break;
    default: msg = ncl_message_new(type); break;
    }
    if (msg == NULL) {
        return NULL;
    }

    switch (type) {
    case NCL_MSG_QUERY_REQUEST:
    case NCL_MSG_QUERY_RESPONSE:
    case NCL_MSG_SET_REQUEST:
    case NCL_MSG_SET_RESPONSE:
    case NCL_MSG_SAMPLE:
        break;
    case NCL_MSG_PING:
        break;
    case NCL_MSG_PONG:
        msg->as.pong.open_api_schema = ncl_msg_read_string(json, "OpenApiSchema");
        break;
    case NCL_MSG_PROBE_VERSION:
        msg->as.probe_version.version = ncl_msg_read_string(json, "version");
        break;
    case NCL_MSG_REGISTER_REQUEST:
        msg->as.register_request.device_id = ncl_msg_read_string(json, "deviceid");
        break;
    case NCL_MSG_REGISTER_RESPONSE:
        msg->as.register_response.code = ncl_msg_read_string(json, "code");
        break;
    case NCL_MSG_PROBE_QUERY_REQUEST:
        break;
    case NCL_MSG_PROBE_QUERY_RESPONSE:
        msg->as.probe_query_response.code = ncl_msg_read_string(json, "code");
        msg->as.probe_query_response.reason = ncl_msg_read_string(json, "reason");
        {
            ncl_json *probe = ncl_json_obj_get(json, "probe");
            if (probe != NULL) {
                msg->as.probe_query_response.model =
                    ncl_root_node_from_json(probe);
            }
        }
        break;
    case NCL_MSG_PROBE_SET_REQUEST:
        {
            ncl_json *probe = ncl_json_obj_get(json, "probe");
            if (probe != NULL) {
                msg->as.probe_set_request.model = ncl_root_node_from_json(probe);
            }
        }
        break;
    case NCL_MSG_PROBE_SET_RESPONSE:
        msg->as.probe_set_response.code = ncl_msg_read_string(json, "code");
        msg->as.probe_set_response.reason = ncl_msg_read_string(json, "reason");
        break;
    case NCL_MSG_EVENT:
        msg->as.event.id = ncl_msg_read_string(json, "id");
        msg->as.event.time = ncl_msg_read_number_as_string(json, "time");
        {
            ncl_json *event = ncl_json_obj_get(json, "event");
            if (event != NULL) {
                msg->as.event.event = ncl_json_clone(event);
            }
        }
        break;
    case NCL_MSG_METHOD_CALL_REQUEST:
        msg->as.method_call_request.method = ncl_msg_read_string(json, "method");
        ncl_item_read_params_into(json, &msg->as.method_call_request.params);
        if (ncl_json_obj_has(json, "check")) {
            msg->as.method_call_request.has_check =
                ncl_json_as_bool(ncl_json_obj_get(json, "check"),
                                 &msg->as.method_call_request.check);
        }
        msg->as.method_call_request.token = ncl_msg_read_string(json, "token");
        break;
    case NCL_MSG_METHOD_CALL_RESPONSE:
        msg->as.method_call_response.code = ncl_msg_read_string(json, "code");
        msg->as.method_call_response.method = ncl_msg_read_string(json, "method");
        ncl_item_read_params_into(json, &msg->as.method_call_response.params);
        if (ncl_json_obj_has(json, "check")) {
            msg->as.method_call_response.has_check =
                ncl_json_as_bool(ncl_json_obj_get(json, "check"),
                                 &msg->as.method_call_response.check);
        }
        msg->as.method_call_response.token = ncl_msg_read_string(json, "token");
        {
            ncl_json *data = ncl_json_obj_get(json, "data");
            if (data != NULL) {
                msg->as.method_call_response.data = ncl_json_clone(data);
            }
        }
        msg->as.method_call_response.reason = ncl_msg_read_string(json, "reason");
        break;
    default:
        ncl_message_free(msg);
        return NULL;
    }

    msg->message_id = ncl_msg_read_string(json, "@id");
    return msg;
}

ncl_message *ncl_message_parse(const char *topic, const char *payload,
                               size_t payload_len)
{
    ncl_msg_type type;
    ncl_json *json;
    ncl_message *msg;
    char *empty_payload = NULL;
    const char *text = payload;

    if (topic == NULL) {
        return NULL;
    }
    type = ncl_msg_type_from_topic(topic);
    if (type == NCL_MSG_UNKNOWN) {
        ncl_log_error("未知消息类型: %s", topic);
        return NULL;
    }

    /* An empty payload is treated as "{}". */
    if (text == NULL || payload_len == 0) {
        empty_payload = ncl_strdup("{}");
        text = empty_payload;
        payload_len = 2;
    }

    json = ncl_json_parse(text, payload_len, NULL);
    free(empty_payload);
    if (json == NULL) {
        ncl_log_error("接收消息出错: JSON解析失败 topic=%s", topic);
        return NULL;
    }
    msg = ncl_message_from_json(type, json);
    ncl_json_free(json);
    if (msg == NULL) {
        ncl_log_error("接收消息出错: topic=%s", topic);
    }
    return msg;
}
