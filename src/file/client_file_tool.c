/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - client side file tool.
 *
 * Everything here runs on the peer side: the NC-Link protocol asks the device
 * to move a file and the bytes land under <cwd>/<sn>/<path>.
 */
#include "nclink/ncl_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

#if defined(NCL_OS_WINDOWS)
#  include <direct.h>
#else
#  include <unistd.h>
#endif

/** Path of the /CONTROLLER/FILE node. */
#define NCL_FILE_NODE_PATH "/CONTROLLER/FILE"

struct ncl_file_client_tool {
    ncl_client *client; /**< borrowed */
};

ncl_file_client_tool *ncl_file_client_tool_create(ncl_client *client)
{
    ncl_file_client_tool *tool;

    if (client == NULL) {
        return NULL;
    }
    tool = (ncl_file_client_tool *)ncl_mem_calloc(1, sizeof(*tool));
    if (tool == NULL) {
        return NULL;
    }
    tool->client = client;
    return tool;
}

void ncl_file_client_tool_free(ncl_file_client_tool *tool)
{
    ncl_mem_free(tool);
}

/* --------------------------------------------------------------- helpers -- */

static void current_dir(char *out, size_t out_len)
{
#if defined(NCL_OS_WINDOWS)
    if (_getcwd(out, (int)out_len) == NULL) {
        snprintf(out, out_len, ".");
    }
#else
    if (getcwd(out, out_len) == NULL) {
        snprintf(out, out_len, ".");
    }
#endif
}

/**
 * The id sent for /CONTROLLER/FILE. The id lookup returns null when the device
 * model does not carry the node; ll() hard-coded
 * the literal path instead. This port always falls back to the literal so that
 * the file channel keeps working with a plain model.
 */
static char *file_node_id(ncl_client *client)
{
    char *id = ncl_client_get_id(client, NCL_FILE_NODE_PATH);
    if (id == NULL) {
        id = ncl_strdup(NCL_FILE_NODE_PATH);
    }
    return id;
}

/** <cwd>/<sn><remote> with local separators. */
static void to_local_path(const ncl_client *client, const char *remote,
                          char *out, size_t out_len)
{
    char root[2048];
    size_t used;
    const char *cursor;

    current_dir(root, sizeof(root));
    snprintf(out, out_len, "%s%c%s", root, NCL_PATH_SEP,
             ncl_client_sn(client) != NULL ? ncl_client_sn(client) : "");
    used = strlen(out);
    for (cursor = remote != NULL ? remote : ""; *cursor != '\0'; cursor++) {
        char c = (*cursor == '/' || *cursor == '\\') ? NCL_PATH_SEP : *cursor;
        if (used + 2 >= out_len) {
            out[used] = '\0';
            return;
        }
        out[used++] = c;
        out[used] = '\0';
    }
}

/** Compare the checksum of the peer's copy with the local one. */
static bool checksum_matches(ncl_file_client_tool *tool, const char *remote)
{
    ncl_ptrvec attributes;
    const ncl_file_attribute *remote_attribute;
    ncl_file_attribute *local_attribute = NULL;
    char path[NCL_PATH_MAX_BUF];
    bool matches = false;

    ncl_ptrvec_init(&attributes, ncl_file_attribute_release);
    if (ncl_file_client_tool_ll(tool, remote, &attributes) != NCL_OK ||
        attributes.len == 0) {
        ncl_ptrvec_free(&attributes);
        return false;
    }
    remote_attribute = (const ncl_file_attribute *)attributes.items[0];
    if (remote_attribute == NULL || remote_attribute->checksum == NULL) {
        ncl_ptrvec_free(&attributes);
        return false;
    }
    to_local_path(tool->client, remote, path, sizeof(path));
    if (ncl_path_exists(path) && !ncl_path_is_dir(path) &&
        ncl_file_attribute_of(path, NULL, &local_attribute) == NCL_OK &&
        local_attribute != NULL && local_attribute->checksum != NULL) {
        matches =
            strcmp(remote_attribute->checksum, local_attribute->checksum) == 0;
    }
    ncl_file_attribute_free(local_attribute);
    ncl_ptrvec_free(&attributes);
    return matches;
}

bool ncl_file_client_tool_detect(ncl_file_client_tool *tool)
{
    char path[NCL_PATH_MAX_BUF];

    if (tool == NULL || tool->client == NULL) {
        return false;
    }
    to_local_path(tool->client, "", path, sizeof(path));
    if (!ncl_path_exists(path)) {
        ncl_mkdir_p(path);
    }
    return true;
}

/* ------------------------------------------------------------- requests --- */

/** Build a SetRequest with a single item. @p value may be NULL (omitted). */
static ncl_message *build_set(ncl_client *client, const char *key,
                              ncl_operation operation, const char *value,
                              long long length)
{
    ncl_message *request;
    ncl_set_request_item *item;
    char *id = file_node_id(client);

    if (id == NULL) {
        return NULL;
    }
    request = ncl_message_new(NCL_MSG_SET_REQUEST);
    if (request == NULL) {
        ncl_mem_free(id);
        return NULL;
    }
    ncl_message_finalise(request);
    item = ncl_set_request_item_new(id);
    ncl_mem_free(id);
    if (item == NULL) {
        ncl_message_free(request);
        return NULL;
    }
    ncl_params_set_string(&item->params, "key", key);
    if (operation != NCL_OP_SET_VALUE) {
        ncl_params_set_string(&item->params, "operation",
                              ncl_operation_to_string(operation));
    }
    if (length >= 0) {
        ncl_params_set_int(&item->params, "offset", 0);
        ncl_params_set_int(&item->params, "length", length);
    }
    if (value != NULL) {
        ncl_params_set_string(&item->params, "value", value);
    }
    ncl_message_add_set_request_item(request, item);
    return request;
}

/** Build a QueryRequest with a single item and one key. */
static ncl_message *build_query(ncl_client *client, ncl_operation operation,
                                const char *key, long long length)
{
    ncl_message *request;
    ncl_query_request_item *item;
    char *id = file_node_id(client);

    if (id == NULL) {
        return NULL;
    }
    request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    if (request == NULL) {
        ncl_mem_free(id);
        return NULL;
    }
    ncl_message_finalise(request);
    item = ncl_query_request_item_new(id);
    ncl_mem_free(id);
    if (item == NULL) {
        ncl_message_free(request);
        return NULL;
    }
    if (operation != NCL_OP_GET_VALUE) {
        ncl_params_set_string(&item->params, "operation",
                              ncl_operation_to_string(operation));
    }
    if (length >= 0) {
        ncl_params_set_int(&item->params, "offset", 0);
        ncl_params_set_int(&item->params, "length", length);
    }
    ncl_params_append_string(&item->params, "keys", key);
    ncl_message_add_query_request_item(request, item);
    return request;
}

/** 0 = OK, 1 = NG, 2 = PENDING, -1 = anything else. */
static int set_status_of(const ncl_message *response)
{
    const ncl_set_response_item *item;

    if (response == NULL || response->type != NCL_MSG_SET_RESPONSE ||
        response->as.set_response.items.len == 0) {
        return -1;
    }
    item =
        (const ncl_set_response_item *)response->as.set_response.items.items[0];
    if (item == NULL || item->code == NULL) {
        return -1;
    }
    if (ncl_check_is_code_ok(item->code)) {
        return 0;
    }
    if (ncl_check_is_code_ng(item->code)) {
        return 1;
    }
    if (ncl_check_is_pending(item->code)) {
        return 2;
    }
    return -1;
}

static int query_status_of(const ncl_message *response)
{
    const ncl_query_response_item *item;

    if (response == NULL || response->type != NCL_MSG_QUERY_RESPONSE ||
        response->as.query_response.items.len == 0) {
        return -1;
    }
    item = (const ncl_query_response_item *)
        response->as.query_response.items.items[0];
    if (item == NULL || item->code == NULL) {
        return -1;
    }
    if (ncl_check_is_code_ok(item->code)) {
        return 0;
    }
    if (ncl_check_is_code_ng(item->code)) {
        return 1;
    }
    if (ncl_check_is_pending(item->code)) {
        return 2;
    }
    return -1;
}

/* ----------------------------------------------------------------- write -- */

bool ncl_file_client_tool_write(ncl_file_client_tool *tool,
                                const char *remote_file_path)
{
    char path[NCL_PATH_MAX_BUF];
    long long size;
    int attempt;

    if (tool == NULL || tool->client == NULL || remote_file_path == NULL) {
        return false;
    }
    if (checksum_matches(tool, remote_file_path)) {
        return true;
    }
    to_local_path(tool->client, remote_file_path, path, sizeof(path));
    if (!ncl_path_exists(path) || ncl_path_is_dir(path)) {
        return false;
    }
    size = ncl_file_size(path);
    if (size < 0) {
        size = 0;
    }
    for (attempt = 0; attempt < 11; attempt++) {
        ncl_message *request =
            build_set(tool->client, remote_file_path, NCL_OP_SET_VALUE,
                      remote_file_path, size);
        ncl_message *response = NULL;
        int status;

        if (request == NULL) {
            return false;
        }
        if (ncl_client_set(tool->client, request, NCL_FILE_OPERATION_TIMEOUT,
                           &response) != NCL_OK) {
            return false;
        }
        status = set_status_of(response);
        ncl_message_free(response);
        if (status == 0) {
            return true;
        }
        if (status != 2) {
            return false;
        }
        ncl_sleep_millis(5000);
    }
    return false;
}

/* ------------------------------------------------------------------ read -- */

char *ncl_file_client_tool_read(ncl_file_client_tool *tool,
                                const char *remote_file_path)
{
    char path[NCL_PATH_MAX_BUF];
    int attempt;

    if (tool == NULL || tool->client == NULL || remote_file_path == NULL) {
        return NULL;
    }
    if (checksum_matches(tool, remote_file_path)) {
        to_local_path(tool->client, remote_file_path, path, sizeof(path));
        return ncl_path_exists(path) ? ncl_strdup(path) : NULL;
    }
    for (attempt = 0; attempt < 11; attempt++) {
        ncl_message *request = build_query(tool->client, NCL_OP_GET_VALUE,
                                           remote_file_path, 2147483647);
        ncl_message *response = NULL;
        int status;

        if (request == NULL) {
            return NULL;
        }
        if (ncl_client_query(tool->client, request, NCL_FILE_OPERATION_TIMEOUT,
                             &response) != NCL_OK) {
            return NULL;
        }
        status = query_status_of(response);
        ncl_message_free(response);
        if (status == 0) {
            to_local_path(tool->client, remote_file_path, path, sizeof(path));
            return ncl_path_exists(path) ? ncl_strdup(path) : NULL;
        }
        if (status != 2) {
            return NULL;
        }
        ncl_sleep_millis(5000);
    }
    return NULL;
}

/* -------------------------------------------------------------------- ll -- */

ncl_err ncl_file_client_tool_ll(ncl_file_client_tool *tool,
                                const char *remote_dir, ncl_ptrvec *out)
{
    ncl_message *request;
    ncl_message *response = NULL;
    ncl_err rc = NCL_ERR;

    if (tool == NULL || tool->client == NULL || out == NULL ||
        remote_dir == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    request = build_query(tool->client, NCL_OP_GET_ATTRIBUTES, remote_dir, -1);
    if (request == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (ncl_client_query(tool->client, request, NCL_FILE_OPERATION_TIMEOUT,
                         &response) != NCL_OK) {
        return NCL_ERR_TIMEOUT;
    }
    if (response != NULL && response->type == NCL_MSG_QUERY_RESPONSE &&
        response->as.query_response.items.len > 0) {
        const ncl_query_response_item *response_item =
            (const ncl_query_response_item *)
                response->as.query_response.items.items[0];
        ncl_json *values = response_item != NULL ? response_item->values : NULL;
        ncl_json *first = values != NULL ? ncl_json_arr_get(values, 0) : NULL;

        if (first != NULL && ncl_json_type_of(first) == NCL_JSON_ARRAY) {
            size_t i;
            for (i = 0; i < ncl_json_arr_len(first); i++) {
                ncl_file_attribute *attribute =
                    ncl_file_attribute_from_json(ncl_json_arr_get(first, i));
                if (attribute != NULL) {
                    ncl_ptrvec_push(out, attribute);
                }
            }
        }
        rc = NCL_OK;
    } else {
        ncl_log_error("查询文件列表失败");
    }
    ncl_message_free(response);
    return rc;
}

/* ----------------------------------------------------------- mkdir / rm --- */

static bool set_simple(ncl_file_client_tool *tool, const char *remote,
                       ncl_operation operation, bool with_value)
{
    ncl_message *request;
    ncl_message *response = NULL;
    bool ok = false;

    request = build_set(tool->client, remote, operation,
                        with_value ? remote : NULL, -1);
    if (request == NULL) {
        return false;
    }
    if (ncl_client_set(tool->client, request, NCL_FILE_OPERATION_TIMEOUT,
                       &response) == NCL_OK) {
        ok = set_status_of(response) == 0;
    }
    ncl_message_free(response);
    return ok;
}

bool ncl_file_client_tool_mkdir(ncl_file_client_tool *tool,
                                const char *remote_dir)
{
    if (tool == NULL || remote_dir == NULL) {
        return false;
    }
    return set_simple(tool, remote_dir, NCL_OP_ADD, true);
}

bool ncl_file_client_tool_delete(ncl_file_client_tool *tool,
                                 const char *remote_file_path)
{
    if (tool == NULL || remote_file_path == NULL) {
        return false;
    }
    return set_simple(tool, remote_file_path, NCL_OP_DELETE, false);
}
