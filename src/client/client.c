/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - per device client. */
#include "nclink/ncl_client.h"
#include "nclink/ncl_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_thread.h"
#include "nclink/ncl_topic.h"
#include "nclink/ncl_env.h"

struct ncl_client {
    char *sn;
    ncl_message_channel *channel; /**< borrowed */

    /* response topics for this serial number */
    char *query_response;
    char *probe_response;
    char *set_response;
    char *probe_set_response;
    char *method_call_response;
    char *edge_response;
    char *event_topic; /**< Event/<sn>, subscribed on demand */
    char *sample_topic; /**< Sample/<sn>/#, subscribed on demand */

    /** messageId (or response topic for Probe) -> ncl_message*, 5 minute TTL. */
    ncl_cache *message_map;
    ncl_mutex *mutex;
    ncl_cond  *cond;

    /** Device model, borrowed (not owned). */
    ncl_node *root_node;

    /** File channel, when one is installed. */
    ncl_file_client_tool *file_tool;

    ncl_client_event_fn event_handler;
    void               *event_user;
    size_t              events;

    ncl_client_sample_fn sample_handler;
    void                *sample_user;
    size_t               samples;
};

static void ncl_client_message_free(void *message)
{
    ncl_message_free((ncl_message *)message);
}

static ncl_err ncl_client_replace_topic(char **slot, char *topic)
{
    if (topic == NULL) {
        return NCL_ERR_NOMEM;
    }
    free(*slot);
    *slot = topic;
    return NCL_OK;
}

ncl_client *ncl_client_create(const char *sn, ncl_message_channel *channel)
{
    ncl_client *client;

    if (sn == NULL || channel == NULL) {
        return NULL;
    }
    client = (ncl_client *)calloc(1, sizeof(ncl_client));
    if (client == NULL) {
        return NULL;
    }
    client->sn = ncl_strdup(sn);
    client->channel = channel;
    client->mutex = ncl_mutex_create();
    client->cond = ncl_cond_create();
    client->message_map = ncl_cache_create(NCL_CLIENT_RESPONSE_TTL_MS, false,
                                           ncl_client_message_free);

    if (client->sn == NULL || client->mutex == NULL || client->cond == NULL ||
        client->message_map == NULL) {
        ncl_client_free(client);
        return NULL;
    }

    /* Response topics this client listens on. */
    ncl_client_replace_topic(&client->query_response, ncl_topic_query_response(sn, NULL));
    ncl_client_replace_topic(&client->probe_response, ncl_topic_probe_query_response(sn, NULL));
    ncl_client_replace_topic(&client->set_response, ncl_topic_set_response(sn, NULL));
    ncl_client_replace_topic(&client->probe_set_response, ncl_topic_probe_set_response(sn, NULL));
    ncl_client_replace_topic(&client->method_call_response, ncl_topic_method_call_response(sn, NULL));
    ncl_client_replace_topic(&client->edge_response, ncl_topic_edge_get_response(sn));
    ncl_client_replace_topic(&client->event_topic, ncl_topic_event(sn, NULL));
    /* A wildcard filter, so one subscription covers every sample channel. */
    {
        char *base = ncl_topic_sample(sn, NULL);
        char *filter = NULL;
        if (base != NULL &&
            ncl_asprintf(&filter, "%s/#", base) == NCL_OK) {
            ncl_client_replace_topic(&client->sample_topic, filter);
        }
        free(base);
    }

    if (client->query_response == NULL || client->probe_response == NULL ||
        client->set_response == NULL || client->probe_set_response == NULL ||
        client->method_call_response == NULL || client->edge_response == NULL ||
        client->event_topic == NULL || client->sample_topic == NULL) {
        ncl_client_free(client);
        return NULL;
    }
    return client;
}

void ncl_client_free(ncl_client *client)
{
    if (client == NULL) {
        return;
    }
    ncl_client_unsubscribe(client);
    ncl_node_free(client->root_node);
    free(client->sn);
    free(client->query_response);
    free(client->probe_response);
    free(client->set_response);
    free(client->probe_set_response);
    free(client->method_call_response);
    free(client->edge_response);
    free(client->event_topic);
    free(client->sample_topic);
    ncl_cache_free(client->message_map);
    ncl_cond_destroy(client->cond);
    ncl_mutex_destroy(client->mutex);
    free(client);
}

const char *ncl_client_sn(const ncl_client *client)
{
    return client != NULL ? client->sn : NULL;
}

ncl_err ncl_client_subscribe(ncl_client *client)
{
    ncl_err rc;
    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* Every response subscription uses QoS 2. */
    rc = ncl_channel_subscribe(client->channel, client->query_response, 2);
    if (rc == NCL_OK) {
        rc = ncl_channel_subscribe(client->channel, client->probe_response, 2);
    }
    if (rc == NCL_OK) {
        rc = ncl_channel_subscribe(client->channel, client->set_response, 2);
    }
    if (rc == NCL_OK) {
        rc = ncl_channel_subscribe(client->channel, client->method_call_response, 2);
    }
    if (rc == NCL_OK) {
        rc = ncl_channel_subscribe(client->channel, client->edge_response, 2);
    }
    if (rc == NCL_OK) {
        rc = ncl_channel_subscribe(client->channel, client->probe_set_response, 2);
    }
    if (rc != NCL_OK) {
        ncl_log_error("订阅响应主题失败: sn=%s", client->sn);
    }
    return rc;
}

ncl_err ncl_client_unsubscribe(ncl_client *client)
{
    if (client == NULL || client->channel == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_channel_unsubscribe(client->channel, client->query_response);
    ncl_channel_unsubscribe(client->channel, client->probe_response);
    ncl_channel_unsubscribe(client->channel, client->set_response);
    ncl_channel_unsubscribe(client->channel, client->method_call_response);
    ncl_channel_unsubscribe(client->channel, client->edge_response);
    ncl_channel_unsubscribe(client->channel, client->probe_set_response);
    return NCL_OK;
}

void ncl_client_on_message(ncl_client *client, const char *topic,
                           ncl_message *message)
{
    char key[512];

    if (client == NULL || message == NULL) {
        ncl_message_free(message);
        return;
    }

    /* Events are not responses: they go to the callback and are released here,
     * so a handler can never leave a dangling message behind. */
    if (topic != NULL && strncmp(topic, NCL_TOPIC_EVENT_PREFIX,
                                strlen(NCL_TOPIC_EVENT_PREFIX)) == 0) {
        if (client->event_handler != NULL) {
            ncl_mutex_lock(client->mutex);
            client->events++;
            ncl_mutex_unlock(client->mutex);
            client->event_handler(client, topic, message, client->event_user);
        } else {
            ncl_log_warn("收到事件但未注册处理器，已丢弃: %s", topic);
        }
        ncl_message_free(message);
        return;
    }

    /* Samples are not responses either: "Sample/<sn>/<通道id>". */
    if (topic != NULL && strncmp(topic, NCL_TOPIC_SAMPLE_PREFIX,
                                strlen(NCL_TOPIC_SAMPLE_PREFIX)) == 0) {
        if (client->sample_handler != NULL) {
            ncl_mutex_lock(client->mutex);
            client->samples++;
            ncl_mutex_unlock(client->mutex);
            client->sample_handler(client, topic, message, client->sample_user);
        } else {
            ncl_log_warn("收到采样但未注册处理器，已丢弃: %s", topic);
        }
        ncl_message_free(message);
        return;
    }

    /* Probe responses are keyed by topic, the rest by "@id". */
    if (topic != NULL && strstr(topic, "Probe") != NULL) {
        snprintf(key, sizeof(key), "%s", topic);
    } else if (message->message_id != NULL) {
        snprintf(key, sizeof(key), "%s", message->message_id);
    } else {
        ncl_log_warn("收到没有 @id 的响应，已丢弃: %s", topic != NULL ? topic : "");
        ncl_message_free(message);
        return;
    }

    if (ncl_cache_put(client->message_map, key, message) != NCL_OK) {
        ncl_message_free(message);
        return;
    }
    ncl_mutex_lock(client->mutex);
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);
}

static ncl_message *ncl_client_take_response(ncl_client *client,
                                             const char *key)
{
    ncl_message *message;

    ncl_mutex_lock(client->mutex);
    /* Detach so the caller owns the message; a plain remove() would free it. */
    message = (ncl_message *)ncl_cache_take(client->message_map, key);
    ncl_mutex_unlock(client->mutex);
    return message;
}

/**
 * Publish a request and wait for the correlated response. @p cache_key selects
 * the correlation key: the response topic for the Probe exchange, NULL to use
 * the request's "@id".
 *
 * Takes ownership of @p request. The key is copied out before the request is
 * released, because the key can live inside the request object.
 */
static ncl_err ncl_client_do_request(ncl_client *client, const char *request_topic,
                                     const char *cache_key, ncl_message *request,
                                     unsigned timeout_ms, ncl_message **out)
{
    ncl_err rc;
    int64_t deadline;
    ncl_message *response = NULL;
    char key[512];

    if (client == NULL || request_topic == NULL || request == NULL ||
        out == NULL) {
        ncl_message_free(request);
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;

    rc = ncl_message_finalise(request);
    if (rc != NCL_OK) {
        ncl_message_free(request);
        return rc;
    }
    {
        const char *source = cache_key != NULL ? cache_key : request->message_id;
        if (ncl_str_is_blank(source)) {
            ncl_message_free(request);
            return NCL_ERR_INVALID_MESSAGE_ID;
        }
        snprintf(key, sizeof(key), "%s", source);
    }

    rc = ncl_channel_publish(client->channel, request_topic, request, 2, NULL);
    ncl_message_free(request);
    if (rc != NCL_OK) {
        ncl_log_error("%s 请求发送失败: %s", request_topic, ncl_err_name(rc));
        return rc;
    }

    deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    ncl_mutex_lock(client->mutex);
    for (;;) {
        int64_t now;
        response = (ncl_message *)ncl_cache_get(client->message_map, key);
        if (response != NULL) {
            break;
        }
        now = ncl_time_monotonic_millis();
        if (now >= deadline) {
            break;
        }
        ncl_cond_wait_timeout(client->cond, client->mutex,
                              (unsigned)(deadline - now));
    }
    ncl_mutex_unlock(client->mutex);

    if (response == NULL) {
        ncl_log_warn("%s 请求超时 (%u ms)", request_topic, timeout_ms);
        return NCL_ERR_TIMEOUT;
    }
    response = ncl_client_take_response(client, key);
    if (response == NULL) {
        return NCL_ERR_TIMEOUT;
    }
    *out = response;
    return NCL_OK;
}

/* =============================================================== requests == */

ncl_err ncl_client_ping(ncl_client *client, unsigned timeout_ms, ncl_message **out)
{
    ncl_message *request;
    char *topic;
    ncl_err rc;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    topic = ncl_topic_ping(client->sn);
    request = ncl_message_new(NCL_MSG_PING);
    if (topic == NULL || request == NULL) {
        free(topic);
        ncl_message_free(request);
        return NCL_ERR_NOMEM;
    }
    rc = ncl_message_finalise(request);
    if (rc == NCL_OK) {
        rc = ncl_client_do_request(client, topic, NULL, request,
                                   timeout_ms, out);
    } else {
        ncl_message_free(request);
    }
    free(topic);
    return rc;
}

ncl_err ncl_client_query(ncl_client *client, ncl_message *request,
                         unsigned timeout_ms, ncl_message **out)
{
    char *topic;
    ncl_err rc;

    if (client == NULL || request == NULL) {
        ncl_message_free(request);
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_message_finalise(request);
    if (rc != NCL_OK) {
        ncl_message_free(request);
        return rc;
    }
    topic = ncl_topic_query_request(client->sn, NULL);
    rc = ncl_client_do_request(client, topic, NULL, request,
                               timeout_ms, out);
    free(topic);
    return rc;
}

ncl_err ncl_client_set(ncl_client *client, ncl_message *request,
                       unsigned timeout_ms, ncl_message **out)
{
    char *topic;
    ncl_err rc;

    if (client == NULL || request == NULL) {
        ncl_message_free(request);
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_message_finalise(request);
    if (rc != NCL_OK) {
        ncl_message_free(request);
        return rc;
    }
    topic = ncl_topic_set_request(client->sn, NULL);
    rc = ncl_client_do_request(client, topic, NULL, request,
                               timeout_ms, out);
    free(topic);
    return rc;
}

ncl_err ncl_client_probe(ncl_client *client, unsigned timeout_ms, ncl_message **out)
{
    char *request_topic;
    ncl_message *request;
    ncl_err rc;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    request_topic = ncl_topic_probe_query_request(client->sn, NULL);
    request = ncl_message_new(NCL_MSG_PROBE_QUERY_REQUEST);
    if (request_topic == NULL || request == NULL) {
        free(request_topic);
        ncl_message_free(request);
        return NCL_ERR_NOMEM;
    }
    rc = ncl_message_finalise(request);
    if (rc == NCL_OK) {
        /* A probe response is correlated by its response topic. */
        rc = ncl_client_do_request(client, request_topic, client->probe_response,
                                   request, timeout_ms, out);
    } else {
        ncl_message_free(request);
    }
    free(request_topic);
    return rc;
}

ncl_err ncl_client_probe_set(ncl_client *client, ncl_message *request,
                             unsigned timeout_ms, ncl_message **out)
{
    char *topic;
    ncl_err rc;

    if (client == NULL || request == NULL) {
        ncl_message_free(request);
        return NCL_ERR_INVALID_ARG;
    }
    topic = ncl_topic_probe_set_request(client->sn, NULL);
    rc = ncl_client_do_request(client, topic, NULL, request,
                               timeout_ms, out);
    free(topic);
    return rc;
}

ncl_err ncl_client_method_call(ncl_client *client, ncl_message *request,
                               unsigned timeout_ms, ncl_message **out)
{
    char *topic;
    ncl_err rc;

    if (client == NULL || request == NULL) {
        ncl_message_free(request);
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_message_finalise(request);
    if (rc != NCL_OK) {
        ncl_message_free(request);
        return rc;
    }
    topic = ncl_topic_method_call_request(client->sn, NULL);
    rc = ncl_client_do_request(client, topic, NULL, request,
                               timeout_ms, out);
    free(topic);
    return rc;
}

/* ============================================================ file channel == */

void ncl_client_set_event_handler(ncl_client *client, ncl_client_event_fn fn,
                                  void *user)
{
    if (client != NULL) {
        client->event_handler = fn;
        client->event_user = user;
    }
}

ncl_err ncl_client_subscribe_events(ncl_client *client, int qos)
{
    if (client == NULL || client->event_topic == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_channel_subscribe(client->channel, client->event_topic, qos);
}

ncl_err ncl_client_unsubscribe_events(ncl_client *client)
{
    if (client == NULL || client->event_topic == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_channel_unsubscribe(client->channel, client->event_topic);
}

size_t ncl_client_event_count(const ncl_client *client)
{
    return client != NULL ? client->events : 0;
}

void ncl_client_set_sample_handler(ncl_client *client, ncl_client_sample_fn fn,
                                   void *user)
{
    if (client != NULL) {
        client->sample_handler = fn;
        client->sample_user = user;
    }
}

ncl_err ncl_client_subscribe_samples(ncl_client *client, int qos)
{
    if (client == NULL || client->sample_topic == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_channel_subscribe(client->channel, client->sample_topic, qos);
}

ncl_err ncl_client_unsubscribe_samples(ncl_client *client)
{
    if (client == NULL || client->sample_topic == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_channel_unsubscribe(client->channel, client->sample_topic);
}

size_t ncl_client_sample_count(const ncl_client *client)
{
    return client != NULL ? client->samples : 0;
}

const char *ncl_client_sample_topic(const ncl_client *client)
{
    return client != NULL ? client->sample_topic : NULL;
}

void ncl_client_set_file_tool(ncl_client *client, ncl_file_client_tool *tool)
{
    if (client != NULL) {
        client->file_tool = tool;
    }
}

ncl_file_client_tool *ncl_client_file_tool(ncl_client *client)
{
    return client != NULL ? client->file_tool : NULL;
}

ncl_err ncl_client_write(ncl_client *client, const char *local_file_path)
{
    if (client == NULL || client->file_tool == NULL) {
        return NCL_ERR_STATE;
    }
    return ncl_file_client_tool_write(client->file_tool, local_file_path)
               ? NCL_OK
               : NCL_ERR;
}

char *ncl_client_read(ncl_client *client, const char *remote_file_path)
{
    if (client == NULL || client->file_tool == NULL) {
        return NULL;
    }
    return ncl_file_client_tool_read(client->file_tool, remote_file_path);
}

ncl_err ncl_client_ll(ncl_client *client, const char *remote_dir,
                      ncl_ptrvec *out)
{
    if (client == NULL || client->file_tool == NULL) {
        return NCL_ERR_STATE;
    }
    return ncl_file_client_tool_ll(client->file_tool, remote_dir, out);
}

/** Basename of a local path, both separators accepted. */
static const char *ncl_client_basename(const char *path)
{
    const char *slash;
    const char *back;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    back = strrchr(path, '\\');
    if (back != NULL && (slash == NULL || back > slash)) {
        slash = back;
    }
    return slash != NULL ? slash + 1 : path;
}

/** <cwd>/<sn>/temp/<name>, the staging file a method call uses. */
static void ncl_client_temp_path(const ncl_client *client, const char *name,
                                 char *out, size_t out_len)
{
    snprintf(out, out_len, "%s%c%s%ctemp%c%s", ncl_env_root(), NCL_PATH_SEP,
             client->sn != NULL ? client->sn : "", NCL_PATH_SEP, NCL_PATH_SEP,
             name != NULL ? name : "");
}

ncl_err ncl_client_method_call_file(ncl_client *client, ncl_message *request,
                                    const char *const *file_keys,
                                    const char *const *file_paths,
                                    size_t file_count, unsigned timeout_ms,
                                    ncl_message **out)
{
    ncl_json *params;
    size_t i;
    ncl_err rc;

    if (client == NULL || request == NULL ||
        request->type != NCL_MSG_METHOD_CALL_REQUEST) {
        ncl_message_free(request);
        return NCL_ERR_INVALID_ARG;
    }
    params = request->as.method_call_request.params;
    if (params != NULL && ncl_json_type_of(params) == NCL_JSON_OBJECT) {
        for (i = 0; i < file_count; i++) {
            const char *path = file_paths != NULL ? file_paths[i] : NULL;
            const char *key = file_keys != NULL ? file_keys[i] : NULL;
            const char *name;
            char staging[NCL_PATH_MAX_BUF];
            char token[NCL_PATH_MAX_BUF];
            char parent[NCL_PATH_MAX_BUF];
            char *slash;

            if (path == NULL || key == NULL) {
                continue;
            }
            name = ncl_client_basename(path);
            ncl_client_temp_path(client, name, staging, sizeof(staging));
            snprintf(parent, sizeof(parent), "%s", staging);
            slash = strrchr(parent, NCL_PATH_SEP);
            if (slash != NULL) {
                *slash = '\0';
                ncl_mkdir_p(parent);
            }
            if (ncl_file_copy(path, staging) != NCL_OK) {
                ncl_log_error("无法暂存文件: %s", path);
                continue;
            }
            snprintf(token, sizeof(token), "%s/%s", NCL_FILE_TEMP_DIR,
                     name != NULL ? name : "");
            /* Push the bytes over the NC-Link file channel, then hand the peer
             * the token instead of the local path. */
            if (!ncl_file_client_tool_write(client->file_tool, token)) {
                ncl_log_error("文件上传失败: %s", token);
            }
            ncl_json_obj_set_string(params, key, token);
        }
    }

    rc = ncl_client_method_call(client, request, timeout_ms, out);
    if (rc != NCL_OK || out == NULL || *out == NULL) {
        return rc;
    }
    if (client->file_tool != NULL) {
        /* The method call response, not the query response. */
        ncl_json *data = (*out)->type == NCL_MSG_METHOD_CALL_RESPONSE
                             ? (*out)->as.method_call_response.data
                             : NULL;
        ncl_json *file_keys_json =
            data != NULL ? ncl_json_obj_get(data, "fileKeys") : NULL;
        for (i = 0; file_keys_json != NULL &&
                    i < ncl_json_arr_len(file_keys_json);
             i++) {
            const char *key =
                ncl_json_as_string(ncl_json_arr_get(file_keys_json, i));
            const char *token;
            char *local;

            if (key == NULL) {
                continue;
            }
            token = ncl_json_obj_get_string(data, key);
            if (token == NULL) {
                continue;
            }
            local = ncl_file_client_tool_read(client->file_tool, token);
            if (local != NULL) {
                ncl_json_obj_set(data, key, ncl_json_new_string(local));
                free(local);
            } else {
                ncl_json_obj_set(data, key, ncl_json_new_null());
            }
        }
    }
    return rc;
}

/* ============================================================ convenience == */

/** Run a single item query and return the response message. */
static ncl_err ncl_client_query_single(ncl_client *client, const char *path,
                                       ncl_operation operation, int has_range,
                                       int start, int end, unsigned timeout_ms,
                                       ncl_message **out)
{
    ncl_query_request_item *item;
    ncl_message *request;
    ncl_err rc;

    if (path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    item = ncl_query_request_item_new(path);
    if (item == NULL) {
        return NCL_ERR_NOMEM;
    }
    rc = ncl_params_set_string(&item->params, "operation",
                               ncl_operation_to_string(operation));
    if (rc == NCL_OK && has_range) {
        char range[64];
        if (start == end) {
            snprintf(range, sizeof(range), "%d", start);
            rc = ncl_params_append_string(&item->params, "indexes", range);
        } else {
            snprintf(range, sizeof(range), "%d-%d", start, end);
            rc = ncl_params_append_string(&item->params, "indexes", range);
        }
    }
    if (rc != NCL_OK) {
        ncl_query_request_item_free(item);
        return rc;
    }

    request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    if (request == NULL) {
        ncl_query_request_item_free(item);
        return NCL_ERR_NOMEM;
    }
    if (ncl_message_add_query_request_item(request, item) != NCL_OK) {
        ncl_message_free(request);
        return NCL_ERR_NOMEM;
    }
    return ncl_client_query(client, request, timeout_ms, out);
}

/** First value of the first response item, cloned for the caller. */
static ncl_err ncl_client_first_value(ncl_message *response, ncl_json **out)
{
    ncl_query_response_item *item;
    ncl_json *value;

    if (response == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!ncl_message_has_data(response)) {
        return NCL_ERR_NOT_FOUND;
    }
    item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
    value = ncl_query_response_item_data(item);
    if (value == NULL) {
        return NCL_ERR_NOT_FOUND;
    }
    *out = ncl_json_clone(value);
    return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

ncl_err ncl_client_get_value(ncl_client *client, const char *path,
                             unsigned timeout_ms, ncl_json **out)
{
    ncl_message *response = NULL;
    ncl_err rc = ncl_client_query_single(client, path, NCL_OP_GET_VALUE, 0, 0, 0,
                                         timeout_ms, &response);

    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_client_first_value(response, out);
    ncl_message_free(response);
    return rc;
}

ncl_err ncl_client_get_value_range(ncl_client *client, const char *path,
                                   int start, int end, unsigned timeout_ms,
                                   ncl_json **out)
{
    ncl_message *response = NULL;
    ncl_err rc = ncl_client_query_single(client, path, NCL_OP_GET_VALUE, 1, start,
                                         end, timeout_ms, &response);

    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_client_first_value(response, out);
    ncl_message_free(response);
    return rc;
}

ncl_err ncl_client_get_length(ncl_client *client, const char *path,
                              unsigned timeout_ms, long long *out_length)
{
    ncl_message *response = NULL;
    ncl_query_response_item *item;
    ncl_json *value;
    ncl_err rc = ncl_client_query_single(client, path, NCL_OP_GET_LENGTH, 0, 0, 0,
                                         timeout_ms, &response);

    if (rc != NCL_OK) {
        return rc;
    }
    if (out_length == NULL || !ncl_message_has_data(response)) {
        ncl_message_free(response);
        return NCL_ERR_NOT_FOUND;
    }
    item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
    value = ncl_query_response_item_data(item);
    if (value == NULL || !ncl_json_as_int(value, out_length)) {
        ncl_message_free(response);
        return NCL_ERR_NOT_FOUND;
    }
    ncl_message_free(response);
    return NCL_OK;
}

static ncl_err ncl_client_set_value_internal(ncl_client *client, const char *path,
                                            ncl_json *value, bool has_index,
                                            int index, unsigned timeout_ms)
{
    ncl_set_request_item *item;
    ncl_message *request;
    ncl_message *response = NULL;
    ncl_err rc;

    if (client == NULL || path == NULL) {
        ncl_json_free(value);
        return NCL_ERR_INVALID_ARG;
    }
    item = ncl_set_request_item_new(path);
    if (item == NULL) {
        ncl_json_free(value);
        return NCL_ERR_NOMEM;
    }
    rc = ncl_params_set_string(&item->params, "operation", "set_value");
    if (rc == NCL_OK && has_index) {
        rc = ncl_params_set_int(&item->params, "index", index);
    }
    if (rc == NCL_OK) {
        rc = ncl_params_set(&item->params, "value", value);
    } else {
        ncl_json_free(value);
    }
    if (rc != NCL_OK) {
        ncl_set_request_item_free(item);
        return rc;
    }

    request = ncl_message_new(NCL_MSG_SET_REQUEST);
    if (request == NULL) {
        ncl_set_request_item_free(item);
        return NCL_ERR_NOMEM;
    }
    if (ncl_message_add_set_request_item(request, item) != NCL_OK) {
        ncl_message_free(request);
        return NCL_ERR_NOMEM;
    }
    rc = ncl_client_set(client, request, timeout_ms, &response);
    if (rc != NCL_OK) {
        return rc;
    }
    /* Report the real result: every response item must be "OK". */
    rc = NCL_OK;
    if (ncl_message_item_count(response) == 0) {
        rc = NCL_ERR_INVALID_MESSAGE;
    } else {
        size_t i;
        for (i = 0; i < ncl_message_item_count(response); i++) {
            const ncl_set_response_item *result =
                (const ncl_set_response_item *)ncl_message_item_at(response, i);
            if (!ncl_check_is_code_ok(result->code)) {
                rc = NCL_ERR_IO;
                break;
            }
        }
    }
    ncl_message_free(response);
    return rc;
}

ncl_err ncl_client_set_value(ncl_client *client, const char *path,
                             ncl_json *value, unsigned timeout_ms)
{
    return ncl_client_set_value_internal(client, path, value, false, 0, timeout_ms);
}

ncl_err ncl_client_set_value_index(ncl_client *client, const char *path,
                                   ncl_json *value, int index,
                                   unsigned timeout_ms)
{
    return ncl_client_set_value_internal(client, path, value, true, index,
                                        timeout_ms);
}

/** Build and issue a "/nclinkServer/<method>" method call. */
static ncl_err ncl_client_server_method(ncl_client *client, const char *method,
                                        ncl_json *params, unsigned timeout_ms)
{
    ncl_message *request;
    ncl_message *response = NULL;
    ncl_err rc;

    request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    if (request == NULL) {
        ncl_json_free(params);
        return NCL_ERR_NOMEM;
    }
    ncl_message_set_method(request, method);
    ncl_message_set_check(request, false); /* "check" stays false unless asked for */
    if (params != NULL) {
        ncl_message_set_params(request, params);
    }
    rc = ncl_client_method_call(client, request, timeout_ms, &response);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_check_is_code_ok(response->as.method_call_response.code)
             ? NCL_OK
             : NCL_ERR_IO;
    ncl_message_free(response);
    return rc;
}

ncl_err ncl_client_add_sample(ncl_client *client, const ncl_node *config,
                              unsigned timeout_ms)
{
    ncl_json *params;

    if (config == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_json_obj_set(params, "request", ncl_node_to_json(config));
    return ncl_client_server_method(client, "/nclinkServer/addSample", params,
                                    timeout_ms);
}

ncl_err ncl_client_remove_sample(ncl_client *client, const char *id,
                                 unsigned timeout_ms)
{
    ncl_json *params;

    if (id == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_json_obj_set_string(params, "id", id);
    return ncl_client_server_method(client, "/nclinkServer/removeSample", params,
                                    timeout_ms);
}

/* ================================================================ model === */

ncl_node *ncl_client_root_node(const ncl_client *client)
{
    return client != NULL ? client->root_node : NULL;
}

void ncl_client_set_root_node(ncl_client *client, ncl_node *root_node)
{
    if (client == NULL) {
        ncl_node_free(root_node);
        return;
    }
    /* Ownership transfers to the client. */
    ncl_node_free(client->root_node);
    client->root_node = root_node;
}

char *ncl_client_get_id(ncl_client *client, const char *path)
{
    ncl_node_map map;
    ncl_node *node;
    char *id = NULL;

    if (client == NULL || client->root_node == NULL || path == NULL) {
        return NULL;
    }
    ncl_node_map_init(&map);
    if (ncl_root_node_path_map(client->root_node, &map) == NCL_OK) {
        node = ncl_node_map_get(&map, path);
        if (node != NULL && node->id != NULL) {
            id = ncl_strdup(node->id);
        }
    }
    ncl_node_map_free(&map);
    return id;
}

char *ncl_client_get_path(ncl_client *client, const char *id)
{
    ncl_node_map map;
    ncl_node *node;
    char *path = NULL;

    if (client == NULL || client->root_node == NULL || id == NULL) {
        return NULL;
    }
    ncl_node_map_init(&map);
    if (ncl_root_node_id_map(client->root_node, &map) == NCL_OK) {
        node = ncl_node_map_get(&map, id);
        if (node != NULL && ncl_node_path(node) != NULL) {
            path = ncl_strdup(ncl_node_path(node));
        }
    }
    ncl_node_map_free(&map);
    return path;
}

bool ncl_client_is_ready(ncl_client *client)
{
    return client != NULL && client->channel != NULL;
}
