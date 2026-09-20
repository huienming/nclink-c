/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - the "file" tool bound to /CONTROLLER/FILE on the device side,
 * plus the FTP endpoint the device itself can serve (ncl_server_start_ftp).
 *
 * The peer side of the channel - the process wide FTP endpoint and the
 * openFileChannel / closeFileChannel handshake - lives in file_channel.c.
 */
#include "file_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_socket.h"

/** State shared by the "file" tool and the FTP endpoint of one server. */
typedef struct {
    ncl_server_file_tool *remote; /**< FTP client to the peer */
    ncl_ftp_server       *ftp;    /**< FTP endpoint of the device */
    char                 *sn;
    bool                  ftp_owned;
    /* 显式指定的对端（ncl_server_set_file_peer）；host 非空时不再按 conf/mqtt.cfg 推 */
    char                 *peer_host;
    unsigned              peer_port;
    char                 *peer_user;
    char                 *peer_password;
    /*
     * 文件通道（客户端用 file/openFileChannel 握手开的租约）。channel_id 非空
     * 才代表"这条对端是握手开的"；静态对端（ncl_server_set_file_peer）没有 id。
     */
    char                 *channel_id;
    int64_t               channel_opened_ms;
    int64_t               channel_used_ms;
    bool                  passive; /**< 对端要求的传输模式（PASV） */
} ncl_file_tool_state;

/* Defined with the state wiring further down; the channel handshake (which
 * comes first in this file) already needs them. */
static void file_state_open_remote_prefix(ncl_file_tool_state *state,
                                          const char *host, unsigned port,
                                          const char *user, const char *password,
                                          const char *prefix);
static void file_state_set_passive(ncl_file_tool_state *state, bool passive);
static ncl_err file_state_require_peer(ncl_file_tool_state *state,
                                       char **reason);

/* --------------------------------------------------------------- helpers -- */

/** Trim ASCII whitespace in place and return the start of @p text. */
static char *trim_in_place(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n')) {
        *--end = '\0';
    }
    return text;
}

/** <root>/uploadFile/<name>, i.e. the installation root plus the upload dir. */
static void upload_path(char *out, size_t out_len, const char *name)
{
    char joined[NCL_PATH_MAX_BUF];
    size_t i;

    snprintf(joined, sizeof(joined), "%s%s", NCL_FILE_UPLOAD_DIR,
             name != NULL ? name : "");
    for (i = 0; joined[i] != '\0'; i++) {
        if (joined[i] == '/') {
            joined[i] = NCL_PATH_SEP;
        }
    }
    snprintf(out, out_len, "%s%s", ncl_env_root(), joined);
}

/**
 * Basename of a path. 走协议时路径是 "/" 分隔的，但标记对象
 * {"@file": "<本地路径>"} 里给的是**本机路径**（Windows 上是 "\"），
 * 两种分隔符都要认，否则临时文件名会变成整个路径、复制必然失败。
 */
static const char *remote_basename(const char *path)
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

/** @p path with its basename removed. */
static void remote_parent(const char *path, char *out, size_t out_len)
{
    const char *name = remote_basename(path);
    size_t len = (name != NULL && name != path) ? (size_t)(name - path) : 0;

    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

static ncl_json *bool_result(bool value)
{
    return ncl_json_new_bool(value);
}

static ncl_json *file_marker(const char *path)
{
    ncl_json *marker = ncl_json_new_object();
    if (marker == NULL) {
        return NULL;
    }
    ncl_json_obj_set_string(marker, NCL_FILE_MARKER, path);
    return marker;
}

/* -------------------------------------------------------- file channel -- */

/** Result of openFileChannel: the channel id plus whether it already existed. */
static ncl_json *channel_result(const char *channel_id, bool reused)
{
    ncl_json *result = ncl_json_new_object();

    if (result == NULL) {
        return NULL;
    }
    ncl_json_obj_set_string(result, "channelId",
                            channel_id != NULL ? channel_id : "");
    ncl_json_obj_set(result, "reused", ncl_json_new_bool(reused));
    return result;
}

/**
 * "openFileChannel": the peer tells the device where to dial for bulk data.
 *
 * Params: host (required), port (required), user (required), password,
 * channelId (required, the lease the peer keeps), path (optional remote prefix,
 * default = this device's SN), force (replace a channel that is already open).
 *
 * Opening twice with the same channelId is a no-op that reports reused=true, so
 * a peer may retry after a broker reconnect without disturbing a transfer that
 * is in flight. A *different* channelId is refused unless force is set, because
 * replacing the endpoint would abort whatever the current peer is doing.
 */
static ncl_err file_tool_open_channel(void *instance, const ncl_json *params,
                                      ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *host = ncl_params_string(params, "host");
    const char *user = ncl_params_string(params, "user");
    const char *password = ncl_params_string(params, "password");
    const char *channel_id = ncl_params_string(params, "channelId");
    const char *path = ncl_params_string(params, "path");
    const char *prefix = NULL;
    long long port = 0;
    ncl_json *force_json = ncl_params_get(params, "force");
    bool force = false;
    bool reused = false;

    if (state == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (force_json != NULL) {
        (void)ncl_json_as_bool(force_json, &force);
    }
    if (ncl_str_is_blank(host) || ncl_str_is_blank(user) ||
        ncl_str_is_blank(channel_id) ||
        !ncl_params_int(params, "port", &port) || port <= 0 || port > 65535) {
        if (reason != NULL) {
            *reason = ncl_strdup("文件通道参数不完整（host/port/user/channelId）");
        }
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_str_is_blank(path)) {
        prefix = path[0] == '/' ? path + 1 : path;
    }

    if (state->remote != NULL && state->channel_id != NULL) {
        if (strcmp(state->channel_id, channel_id) == 0) {
            /* Same lease: refresh the idle clock, do not touch the FTP client. */
            state->channel_used_ms = ncl_time_monotonic_millis();
            *result = channel_result(state->channel_id, true);
            return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
        }
        if (!force) {
            if (reason != NULL) {
                char text[160];
                snprintf(text, sizeof(text),
                         "文件通道已被占用（channelId=%s），force=true 才能顶替",
                         state->channel_id);
                *reason = ncl_strdup(text);
            }
            return NCL_ERR_EXISTS;
        }
    }

    file_state_open_remote_prefix(state, host, (unsigned)port, user, password,
                                  prefix);
    if (state->remote == NULL) {
        if (reason != NULL) {
            *reason = ncl_strdup("文件通道建立失败");
        }
        return NCL_ERR_NOMEM;
    }
    {
        /* The peer knows whether it can be dialled back (NAT, container,
         * firewall): it asks for passive when it cannot. */
        ncl_json *passive_json = ncl_params_get(params, "passive");
        bool passive = false;

        if (passive_json != NULL) {
            (void)ncl_json_as_bool(passive_json, &passive);
        }
        file_state_set_passive(state, passive);
    }
    ncl_mem_free(state->channel_id);
    state->channel_id = ncl_strdup(channel_id);
    if (state->channel_id == NULL) {
        ncl_server_file_tool_free(state->remote);
        state->remote = NULL;
        return NCL_ERR_NOMEM;
    }
    state->channel_opened_ms = ncl_time_monotonic_millis();
    state->channel_used_ms = state->channel_opened_ms;
    ncl_log_info("文件通道已打开: channelId=%s, 对端 %s:%lld, 前缀 /%s",
                 state->channel_id, host, port,
                 prefix != NULL ? prefix : (state->sn != NULL ? state->sn : ""));
    *result = channel_result(state->channel_id, reused);
    return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/**
 * "closeFileChannel": drop the FTP client of the channel. A channelId that does
 * not match the open one is refused; no channel at all is not an error (the
 * call is idempotent, which is what a peer wants when it is tearing down).
 */
static ncl_err file_tool_close_channel(void *instance, const ncl_json *params,
                                       ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *channel_id = ncl_params_string(params, "channelId");
    ncl_json *closed;

    if (state == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (state->channel_id == NULL) {
        /* Nothing handshake opened: a static peer is not ours to close. */
        *result = ncl_json_new_bool(false);
        return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    if (!ncl_str_is_blank(channel_id) &&
        strcmp(state->channel_id, channel_id) != 0) {
        if (reason != NULL) {
            char text[160];
            snprintf(text, sizeof(text),
                     "channelId 不匹配（当前 %s）", state->channel_id);
            *reason = ncl_strdup(text);
        }
        return NCL_ERR_INVALID_ARG;
    }
    ncl_log_info("文件通道已撤销: channelId=%s", state->channel_id);
    ncl_mem_free(state->channel_id);
    state->channel_id = NULL;
    if (state->remote != NULL) {
        /* In-flight operations fail from here on: that is the revocation. */
        ncl_server_file_tool_free(state->remote);
        state->remote = NULL;
    }
    closed = ncl_json_new_bool(true);
    *result = closed;
    return closed != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/* ----------------------------------------------------------- tool methods -- */

/**
 * "write" method of the file tool:
 *   value is absent          -> fetch the file from the peer over FTP;
 *   value is the file name   -> same (that is what the client side sends);
 *   value is the local path  -> it was uploaded through HTTP, so take it.
 */
/**
 * The transfer call: the channel rides with it. A call that names a peer
 * (host / port / user / channelId) opens the channel first - that is the
 * handshake, now part of this call - and then the file moves, so one call
 * does what "openFileChannel" + "write" used to do in two steps.
 */
static ncl_err file_tool_write(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason);

static ncl_err file_tool_transfer(void *instance, const ncl_json *params,
                                  ncl_json **result, char **reason)
{
    if (!ncl_str_is_blank(ncl_params_string(params, "host"))) {
        ncl_json *opened = NULL;
        ncl_err rc = file_tool_open_channel(instance, params, &opened, reason);

        ncl_json_free(opened);
        if (rc != NCL_OK) {
            return rc;
        }
    }
    return file_tool_write(instance, params, result, reason);
}

static ncl_err file_tool_write(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *key;
    const char *value;
    char filename[NCL_PATH_MAX_BUF];
    char target[NCL_PATH_MAX_BUF];
    char *char_p;

    {
        ncl_err peer_rc = file_state_require_peer(state, reason);
        if (peer_rc != NCL_OK) {
            return peer_rc;
        }
    }
    key = ncl_params_string(params, "key");
    if (key == NULL || ncl_str_is_blank(key)) {
        if (reason != NULL) {
            *reason = ncl_strdup("文件名不能为空");
        }
        return NCL_ERR_INVALID_ARG;
    }
    snprintf(filename, sizeof(filename), "%s", key);
    char_p = trim_in_place(filename);
    memmove(filename, char_p, strlen(char_p) + 1);
    upload_path(target, sizeof(target), filename);

    value = ncl_params_string(params, "value");
    if (value == NULL || strcmp(value, filename) == 0 ||
        strcmp(value, target) == 0) {
        char *local = ncl_server_file_tool_read(state->remote, filename);
        if (local == NULL) {
            *result = bool_result(false);
            return NCL_OK;
        }
        ncl_mem_free(local);
        *result = bool_result(true);
        return NCL_OK;
    }
    {
        ncl_err rc = ncl_file_copy(value, target);
        if (rc != NCL_OK) {
            if (reason != NULL) {
                *reason = ncl_strdup("写文件失败");
            }
            return rc;
        }
    }
    *result = bool_result(ncl_path_exists(target));
    return NCL_OK;
}

/**
 * "read" method of the file tool: when localname is given the local copy
 * is returned straight away, otherwise the named file is published to the peer
 * over FTP and its local path is returned.
 */
static ncl_err file_tool_read(void *instance, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *localname = ncl_params_string(params, "localname");
    const char *filename;
    char path[NCL_PATH_MAX_BUF];
    char parent[NCL_PATH_MAX_BUF];

    {
        ncl_err peer_rc = file_state_require_peer(state, reason);
        if (peer_rc != NCL_OK) {
            return peer_rc;
        }
    }
    if (!ncl_str_is_blank(localname)) {
        upload_path(path, sizeof(path), localname);
        *result = file_marker(path);
        return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    {
        ncl_json *keys = ncl_params_get(params, "keys");
        ncl_json *first =
            (keys != NULL && ncl_json_type_of(keys) == NCL_JSON_ARRAY)
                ? ncl_json_arr_get(keys, 0)
                : NULL;
        filename = first != NULL ? ncl_json_as_string(first) : NULL;
    }
    if (filename == NULL) {
        if (reason != NULL) {
            *reason = ncl_strdup("文件名列表不能为空");
        }
        return NCL_ERR_INVALID_ARG;
    }
    upload_path(path, sizeof(path), filename);
    remote_parent(filename, parent, sizeof(parent));
    if (!ncl_server_file_tool_write(state->remote, path, parent)) {
        if (reason != NULL) {
            *reason = ncl_strdup("读文件失败");
        }
        return NCL_ERR_IO;
    }
    if (!ncl_path_exists(path)) {
        if (reason != NULL) {
            *reason = ncl_strdup("文件不存在");
        }
        return NCL_ERR_NOT_FOUND;
    }
    *result = file_marker(path);
    return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

static ncl_err file_tool_ll(void *instance, const ncl_json *params,
                            ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    ncl_json *keys = ncl_params_get(params, "keys");
    ncl_json *array = ncl_json_new_array();
    size_t i;

    {
        ncl_err peer_rc = file_state_require_peer(state, reason);
        if (peer_rc != NCL_OK) {
            return peer_rc;
        }
    }
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; keys != NULL && i < ncl_json_arr_len(keys); i++) {
        const char *filename = ncl_json_as_string(ncl_json_arr_get(keys, i));
        ncl_ptrvec attributes;
        size_t j;

        if (filename == NULL) {
            continue;
        }
        ncl_ptrvec_init(&attributes, ncl_file_attribute_release);
        if (ncl_server_file_tool_ll(state->remote, filename, &attributes) ==
            NCL_OK) {
            for (j = 0; j < attributes.len; j++) {
                ncl_json *item = ncl_file_attribute_to_json(
                    (const ncl_file_attribute *)attributes.items[j]);
                if (item != NULL) {
                    ncl_json_arr_push(array, item);
                }
            }
        }
        ncl_ptrvec_free(&attributes);
    }
    *result = array;
    return NCL_OK;
}

static ncl_err file_tool_mkdir(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *key = ncl_params_string(params, "key");

    {
        ncl_err peer_rc = file_state_require_peer(state, reason);
        if (peer_rc != NCL_OK) {
            return peer_rc;
        }
    }
    if (key == NULL) {
        *result = bool_result(false);
        return NCL_OK;
    }
    *result = bool_result(ncl_server_file_tool_mkdir(state->remote, key));
    return NCL_OK;
}

static ncl_err file_tool_delete(void *instance, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *key = ncl_params_string(params, "key");

    {
        ncl_err peer_rc = file_state_require_peer(state, reason);
        if (peer_rc != NCL_OK) {
            return peer_rc;
        }
    }
    if (key == NULL) {
        *result = bool_result(false);
        return NCL_OK;
    }
    *result = bool_result(ncl_server_file_tool_delete(state->remote, key));
    return NCL_OK;
}

/*
 * Parameter schemas for every tool method. They are consulted when a MethodCall
 * arrives with "check": true, and they also describe the tool to callers.
 * Note that these methods are normally reached through the Set/Query items of
 * "/CONTROLLER/FILE", so the schema deliberately tolerates the extra item
 * keywords (operation, index, ...).
 */
#define FILE_WRITE_SCHEMA                                                      \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"key\":{\"type\":\"string\",\"minLength\":1},"                           \
    "\"value\":{\"type\":\"string\"},"                                         \
    "\"offset\":{\"type\":\"integer\",\"minimum\":0},"                        \
    "\"length\":{\"type\":\"integer\",\"minimum\":0}},"                       \
    "\"required\":[\"key\"]}"

#define FILE_READ_SCHEMA                                                       \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"            \
    "\"minItems\":1},"                                                         \
    "\"localname\":{\"type\":\"string\"}},"                                    \
    "\"anyOf\":[{\"required\":[\"keys\"]},{\"required\":[\"localname\"]}]}"

#define FILE_LL_SCHEMA                                                         \
    "{\"type\":\"object\",\"properties\":{\"keys\":{\"type\":\"array\","       \
    "\"items\":{\"type\":\"string\"}}},\"required\":[\"keys\"]}"

#define FILE_KEY_SCHEMA                                                        \
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\","       \
    "\"minLength\":1}},\"required\":[\"key\"]}"

#define FILE_CHANNEL_OPEN_SCHEMA                                               \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"host\":{\"type\":\"string\",\"minLength\":1},"                          \
    "\"port\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535},"         \
    "\"user\":{\"type\":\"string\",\"minLength\":1},"                          \
    "\"password\":{\"type\":\"string\"},"                                      \
    "\"channelId\":{\"type\":\"string\",\"minLength\":1},"                     \
    "\"path\":{\"type\":\"string\"},"                                          \
    "\"force\":{\"type\":\"boolean\"}},"                                       \
    "\"required\":[\"host\",\"port\",\"user\",\"channelId\"]}"

#define FILE_CHANNEL_CLOSE_SCHEMA                                              \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"channelId\":{\"type\":\"string\"}}}"

static const ncl_tool_method k_file_methods[] = {
    {"write", file_tool_write, FILE_WRITE_SCHEMA},
    {"read", file_tool_read, FILE_READ_SCHEMA},
    {"ll", file_tool_ll, FILE_LL_SCHEMA},
    {"mkdir", file_tool_mkdir, FILE_KEY_SCHEMA},
    {"delete", file_tool_delete, FILE_KEY_SCHEMA},
    {"openFileChannel", file_tool_open_channel, FILE_CHANNEL_OPEN_SCHEMA},
    {"closeFileChannel", file_tool_close_channel, FILE_CHANNEL_CLOSE_SCHEMA}};

/* Every file tool method lives on CONTROLLER/FILE under a different operation,
 * which is exactly the "<operation>#<path>" binding key. */
#define FILE_NODE_PATH "/CONTROLLER/FILE"

static const ncl_tool_binding k_file_bindings[] = {
    {FILE_NODE_PATH, NCL_OP_SET_VALUE, "write", "file"},
    {FILE_NODE_PATH, NCL_OP_GET_VALUE, "read", "file"},
    {FILE_NODE_PATH, NCL_OP_GET_ATTRIBUTES, "ll", "file"},
    {FILE_NODE_PATH, NCL_OP_ADD, "mkdir", "file"},
    {FILE_NODE_PATH, NCL_OP_DELETE, "delete", "file"},
    /* The channel handshake is a function call on the file tool; the binding is
     * what makes it reachable by name through the method call dispatcher. */
    {FILE_NODE_PATH, NCL_OP_FUNC_CALL, "openFileChannel", "file"},
    {FILE_NODE_PATH, NCL_OP_FUNC_CALL, "closeFileChannel", "file"}};

/* ----------------------------------------------------------- state wiring -- */

static void file_state_destroy(void *data)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)data;

    if (state == NULL) {
        return;
    }
    if (state->ftp != NULL) {
        ncl_ftp_server_free(state->ftp);
        state->ftp = NULL;
    }
    if (state->remote != NULL) {
        ncl_server_file_tool_free(state->remote);
        state->remote = NULL;
    }
    ncl_mem_free(state->sn);
    ncl_mem_free(state->peer_host);
    ncl_mem_free(state->peer_user);
    ncl_mem_free(state->peer_password);
    ncl_mem_free(state->channel_id);
    ncl_mem_free(state);
}

/**
 * 用当前生效的对端参数建（或换）FTP 客户端。@p prefix 是远端目录的前缀
 * （协议里的 SN），NULL 表示用本机 SN。
 */
static void file_state_open_remote_prefix(ncl_file_tool_state *state,
                                          const char *host, unsigned port,
                                          const char *user, const char *password,
                                          const char *prefix)
{
    if (state == NULL) {
        return;
    }
    if (state->remote != NULL) {
        ncl_server_file_tool_free(state->remote);
        state->remote = NULL;
    }
    state->remote = ncl_server_file_tool_create(
        host != NULL ? host : "127.0.0.1",
        port != 0 ? port : (unsigned)NCL_FTP_CLIENT_HOLDER_PORT,
        user != NULL && user[0] != '\0' ? user : NCL_FTP_DEFAULT_USER,
        password != NULL && password[0] != '\0' ? password : NCL_FTP_DEFAULT_PASSWORD,
        prefix != NULL ? prefix : state->sn);
    if (state->remote != NULL) {
        ncl_server_file_tool_set_passive(state->remote, state->passive);
    }
}

/** Remember the transfer mode the peer asked for and apply it right away. */
static void file_state_set_passive(ncl_file_tool_state *state, bool passive)
{
    if (state == NULL) {
        return;
    }
    state->passive = passive;
    if (state->remote != NULL) {
        ncl_server_file_tool_set_passive(state->remote, passive);
    }
}

/** ncl_server_file_tool_create() with the device's own SN as the prefix. */
static void file_state_open_remote(ncl_file_tool_state *state, const char *host,
                                   unsigned port, const char *user,
                                   const char *password)
{
    file_state_open_remote_prefix(state, host, port, user, password, NULL);
}

/**
 * The file methods need a peer. An open channel (or a statically configured
 * peer) is what makes state->remote non-NULL; without it there is nothing to
 * talk to and the caller has to open a channel first.
 */
static ncl_err file_state_require_peer(ncl_file_tool_state *state, char **reason)
{
    if (state != NULL && state->remote != NULL) {
        state->channel_used_ms = ncl_time_monotonic_millis();
        return NCL_OK;
    }
    if (reason != NULL) {
        *reason = ncl_strdup("文件通道未打开");
    }
    return NCL_ERR_NO_CHANNEL;
}

/**
 * A state of its own, for the declaration path - there the state is the
 * tool's context. NULL when there is no memory.
 *
 * 对端目录用的是"协议里的 SN"，也就是设备自己的 SN：客户端把文件放在
 * <cwd>/<它寻址的那个 SN>/ 下，而它寻址的就是这个 SN。bin/sn.txt 是
 * "设备从文件里读 SN"那条路用的（ncl_sn_read() 会生成它），两者可能不一样
 * —— 用 bin/sn.txt 的话，显式指定 SN 的设备端就找不到客户端摆好的文件。
 */
static ncl_file_tool_state *file_state_create(const char *sn)
{
    ncl_file_tool_state *state =
        (ncl_file_tool_state *)ncl_mem_calloc(1, sizeof(*state));

    if (state == NULL) {
        return NULL;
    }
    state->sn = ncl_strdup(sn);
    if (state->sn == NULL) {
        ncl_mem_free(state);
        return NULL;
    }
    return state;
}

/**
 * Fetch (creating on demand) the file state of @p server.
 *
 * 对端只有两种来源：ncl_server_set_file_peer() 显式指定的静态对端，或客户端
 * 用 file/openFileChannel 握手开的文件通道。**不再**从 conf/mqtt.cfg 猜 broker
 * 主机 + 2323 + admin/123456 —— 那条隐式路径在 3.4.0 删掉了。
 */
static ncl_file_tool_state *file_state_of(ncl_server *server)
{
    ncl_file_tool_state *state =
        (ncl_file_tool_state *)ncl_server_user_data(server);

    if (state != NULL) {
        return state;
    }
    state = file_state_create(ncl_server_sn(server));
    if (state == NULL) {
        return NULL;
    }
    if (state->peer_host != NULL) {
        file_state_open_remote(state, state->peer_host, state->peer_port,
                               state->peer_user, state->peer_password);
    }
    if (ncl_server_set_user_data(server, state, file_state_destroy) != NCL_OK) {
        file_state_destroy(state);
        return NULL;
    }
    return state;
}

ncl_err ncl_server_register_file_tool(ncl_server *server)
{
    ncl_file_tool_state *state;

    if (server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    state = file_state_of(server);
    if (state == NULL) {
        return NCL_ERR_NOMEM;
    }
    return ncl_server_register_tool(server, "file", state, k_file_methods,
                                    sizeof(k_file_methods) /
                                        sizeof(k_file_methods[0]),
                                    k_file_bindings,
                                    sizeof(k_file_bindings) /
                                        sizeof(k_file_bindings[0]));
}

/* ------------------------------------------------------- the declaration -- */

/**
 * The file point's function. The whole tool is this one dispatch and the
 * operation says which way the file goes:
 *
 *   get_value        read it out (the device pushes it to the peer)
 *   get_attributes   the listing (what "ll" serves)
 *   add / delete     make / remove a directory, remove a file
 *   call             **start a transfer**: the call's own params carry both
 *                    the file and the channel it rides on (host / port /
 *                    user / password / channelId) - the handshake is part of
 *                    the call instead of two methods of its own.
 */
static ncl_err file_point_fn(void *ctx, const ncl_tool_point *self,
                             ncl_operation op, const ncl_json *params,
                             ncl_json **result, char **reason)
{
    (void)self;
    switch (op) {
    case NCL_OP_GET_VALUE:
        return file_tool_read(ctx, params, result, reason);
    case NCL_OP_GET_ATTRIBUTES:
        return file_tool_ll(ctx, params, result, reason);
    case NCL_OP_ADD:
        return file_tool_mkdir(ctx, params, result, reason);
    case NCL_OP_DELETE:
        return file_tool_delete(ctx, params, result, reason);
    case NCL_OP_FUNC_CALL:
        return file_tool_transfer(ctx, params, result, reason);
    default:
        break;
    }
    return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                         "unsupported operation on %s", FILE_NODE_PATH);
}

/** The tool's context: its state, made from the configuration. */
static void *file_tool_open(const ncl_json *params, char **err)
{
    const char *sn = ncl_tool_param_str(params, "sn", NULL);
    char *owned = NULL;
    ncl_file_tool_state *state;

    if (sn == NULL) {
        owned = ncl_sn_read();
        sn = owned;
    }
    state = sn != NULL ? file_state_create(sn) : NULL;
    ncl_free_safe(owned);
    if (state == NULL && err != NULL) {
        *err = ncl_strdup("文件工具起不来（拿不到 SN）");
    }
    return state;
}

static void file_tool_close(void *ctx)
{
    file_state_destroy(ctx);
}

NCL_TOOL_BEGIN("file", "文件传输（NC-Link 文件通道，字节走 FTP）", 0, 0,
               file_tool_open, file_tool_close)
    NCL_CONFIG_OPS("/MACHINE/CONTROLLER/FILE", file_point_fn, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES) |
                       NCL_OP_BIT(NCL_OP_ADD) | NCL_OP_BIT(NCL_OP_DELETE) |
                       NCL_OP_BIT(NCL_OP_FUNC_CALL))
NCL_TOOL_END()

const ncl_tool_decl *ncl_file_tool_declaration(void)
{
    /* Built once: a declaration is a table, and a host registers the very
     * same one with its own server (the state - the context - is per
     * registration, created by file_tool_open()). */
    static ncl_tool_decl declaration;

    if (declaration.points == NULL) {
        declaration = ncl_tool_declaration();
    }
    return &declaration;
}

ncl_err ncl_file_tool_register(ncl_server *server, const ncl_json *params,
                               const ncl_tool_audit *audit,
                               ncl_tool_registration **out, ncl_strbuf *err)
{
    return ncl_tool_register(server, ncl_file_tool_declaration(), params,
                             audit, out, err);
}

ncl_err ncl_server_set_file_peer(ncl_server *server, const char *host, unsigned port,
                                 const char *user, const char *password)
{
    ncl_file_tool_state *state;
    char *host_copy;
    char *user_copy = NULL;
    char *pass_copy = NULL;

    if (server == NULL || host == NULL || host[0] == '\0') {
        return NCL_ERR_INVALID_ARG;
    }
    state = file_state_of(server);
    if (state == NULL) {
        return NCL_ERR_NOMEM;
    }
    host_copy = ncl_strdup(host);
    if (host_copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (user != NULL && user[0] != '\0') {
        user_copy = ncl_strdup(user);
        if (user_copy == NULL) {
            ncl_mem_free(host_copy);
            return NCL_ERR_NOMEM;
        }
    }
    if (password != NULL && password[0] != '\0') {
        pass_copy = ncl_strdup(password);
        if (pass_copy == NULL) {
            ncl_mem_free(host_copy);
            ncl_mem_free(user_copy);
            return NCL_ERR_NOMEM;
        }
    }
    ncl_mem_free(state->peer_host);
    ncl_mem_free(state->peer_user);
    ncl_mem_free(state->peer_password);
    state->peer_host = host_copy;
    state->peer_port = port;
    state->peer_user = user_copy;
    state->peer_password = pass_copy;
    /* 换掉已经建好的对端：下一次传输用新地址重连。 */
    file_state_open_remote(state, state->peer_host, state->peer_port,
                           state->peer_user, state->peer_password);
    return state->remote != NULL ? NCL_OK : NCL_ERR_CONNECT;
}

bool ncl_server_file_channel_is_open(ncl_server *server)
{
    ncl_file_tool_state *state;

    if (server == NULL) {
        return false;
    }
    state = (ncl_file_tool_state *)ncl_server_user_data(server);
    return state != NULL && state->remote != NULL;
}

bool ncl_server_file_channel_id(ncl_server *server, char *out, size_t out_len)
{
    ncl_file_tool_state *state;

    if (server == NULL) {
        return false;
    }
    state = (ncl_file_tool_state *)ncl_server_user_data(server);
    if (state == NULL || state->channel_id == NULL) {
        return false;
    }
    if (out != NULL && out_len > 0) {
        snprintf(out, out_len, "%s", state->channel_id);
    }
    return true;
}

/* ---------------------------------------------------------- FTP endpoint -- */

ncl_err ncl_server_start_ftp(ncl_server *server)
{
    ncl_file_tool_state *state;
    ncl_ftp_response info;
    ncl_ftp_server_options options;
    ncl_err rc;

    if (server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    state = file_state_of(server);
    if (state == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (state->ftp != NULL && ncl_ftp_server_is_running(state->ftp)) {
        return NCL_OK;
    }
    memset(&info, 0, sizeof(info));
    if (ncl_ftp_info_read(&info) != NCL_OK) {
        return NCL_ERR_IO;
    }
    memset(&options, 0, sizeof(options));
    options.port = info.port > 0 ? (unsigned)info.port : NCL_FTP_SERVER_PORT;
    options.root = ncl_env_root();
    options.user = info.user_name;
    options.password = info.password;
    options.allow_write = true;
    /* Log the credentials and the login root. */
    ncl_log_info("FTP服务器已启动,端口:%d", options.port);
    ncl_log_info("用户名:%s,密码:%s",
                 options.user != NULL ? options.user : "",
                 options.password != NULL ? options.password : "");
    ncl_log_info("根目录:%s", options.root);

    state->ftp = ncl_ftp_server_create_ex(&options);
    ncl_ftp_info_free(&info);
    if (state->ftp == NULL) {
        return NCL_ERR_NOMEM;
    }
    rc = ncl_ftp_server_start(state->ftp);
    if (rc != NCL_OK) {
        ncl_ftp_server_free(state->ftp);
        state->ftp = NULL;
    }
    return rc;
}

void ncl_server_stop_ftp(ncl_server *server)
{
    ncl_file_tool_state *state =
        server != NULL ? (ncl_file_tool_state *)ncl_server_user_data(server)
                       : NULL;

    if (state != NULL && state->ftp != NULL) {
        ncl_ftp_server_free(state->ftp);
        state->ftp = NULL;
    }
}

ncl_err ncl_client_holder_restart(void)
{
    ncl_mqtt_config config;
    ncl_err rc;

    memset(&config, 0, sizeof(config));
    if (ncl_mqtt_config_read(&config) != NCL_OK || config.url == NULL) {
        ncl_mqtt_config_free(&config);
        return NCL_ERR_IO;
    }
    ncl_client_holder_shutdown();
    rc = ncl_client_holder_init(config.url, config.username, config.password);
    ncl_mqtt_config_free(&config);
    return rc;
}

/* ------------------------------------------------------ method call files -- */

ncl_json *ncl_file_extract_file_values(ncl_json *data)
{
    ncl_json *file_keys = NULL;
    size_t i;

    if (data == NULL || ncl_json_type_of(data) != NCL_JSON_OBJECT) {
        return data;
    }
    for (i = 0; i < ncl_json_obj_len(data); i++) {
        const char *key = ncl_json_obj_key_at(data, i);
        ncl_json *value = ncl_json_obj_val_at(data, i);
        const char *path;
        const char *name;
        char target[NCL_PATH_MAX_BUF];
        char token[NCL_PATH_MAX_BUF];
        char relative[NCL_PATH_MAX_BUF];

        if (value == NULL || ncl_json_type_of(value) != NCL_JSON_OBJECT ||
            key == NULL || strcmp(key, "fileKeys") == 0) {
            continue;
        }
        path = ncl_json_obj_get_string(value, NCL_FILE_MARKER);
        if (path == NULL) {
            continue;
        }
        name = remote_basename(path);
        /*
         * 落点和文件工具本地的镜像路径一致：<root>/uploadFile/temp/<名字>。
         * 客户端随后按 "/temp/<名字>" 来取（file 工具的 read / get_value），
         * read 找的就是这个镜像路径；放到 <root>/temp/ 下的话它就找不到了。
         */
        snprintf(relative, sizeof(relative), "%s/%s", NCL_FILE_TEMP_DIR,
                 name != NULL ? name : "");
        upload_path(target, sizeof(target), relative);
        snprintf(token, sizeof(token), "%s/%s", NCL_FILE_TEMP_DIR,
                 name != NULL ? name : "");
        {
            ncl_err rc = ncl_file_copy(path, target);

            if (rc != NCL_OK) {
                /* 通常是把结果里的 {"@file": ...} 指到了一个读不到/写不进去的位置 */
                ncl_log_error("无法复制临时文件: %s -> %s (%s)", path, target,
                              ncl_err_name(rc));
                continue;
            }
        }
        ncl_json_obj_set(data, key, ncl_json_new_string(token));
        if (file_keys == NULL) {
            file_keys = ncl_json_new_array();
            if (file_keys == NULL) {
                return data;
            }
        }
        ncl_json_arr_push(file_keys, ncl_json_new_string(key));
    }
    if (file_keys != NULL) {
        ncl_json_obj_set(data, "fileKeys", file_keys);
    }
    return data;
}
