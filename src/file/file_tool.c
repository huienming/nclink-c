/*
 * NC-Link core - the "file" tool bound to /CONTROLLER/FILE, plus the two FTP
 * endpoints (device side and process wide client side).
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
} ncl_file_tool_state;

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

/** Basename of a "/"-separated remote path. */
static const char *remote_basename(const char *path)
{
    const char *slash;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
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

/* ----------------------------------------------------------- tool methods -- */

/**
 * "write" method of the file tool:
 *   value is absent          -> fetch the file from the peer over FTP;
 *   value is the file name   -> same (that is what the client side sends);
 *   value is the local path  -> it was uploaded through HTTP, so take it.
 */
static ncl_err file_tool_write(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    ncl_file_tool_state *state = (ncl_file_tool_state *)instance;
    const char *key;
    const char *value;
    char filename[NCL_PATH_MAX_BUF];
    char target[NCL_PATH_MAX_BUF];
    char *char_p;

    (void)reason;
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
        free(local);
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

    (void)reason;
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

    (void)reason;
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

    (void)reason;
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

    (void)reason;
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

static const ncl_tool_method k_file_methods[] = {
    {"write", file_tool_write, FILE_WRITE_SCHEMA},
    {"read", file_tool_read, FILE_READ_SCHEMA},
    {"ll", file_tool_ll, FILE_LL_SCHEMA},
    {"mkdir", file_tool_mkdir, FILE_KEY_SCHEMA},
    {"delete", file_tool_delete, FILE_KEY_SCHEMA}};

/* Every file tool method lives on CONTROLLER/FILE under a different operation,
 * which is exactly the "<operation>#<path>" binding key. */
#define FILE_NODE_PATH "/CONTROLLER/FILE"

static const ncl_tool_binding k_file_bindings[] = {
    {FILE_NODE_PATH, NCL_OP_SET_VALUE, "write", "file"},
    {FILE_NODE_PATH, NCL_OP_GET_VALUE, "read", "file"},
    {FILE_NODE_PATH, NCL_OP_GET_ATTRIBUTES, "ll", "file"},
    {FILE_NODE_PATH, NCL_OP_ADD, "mkdir", "file"},
    {FILE_NODE_PATH, NCL_OP_DELETE, "delete", "file"}};

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
    free(state->sn);
    free(state);
}

/**
 * Fetch (creating on demand) the file state of @p server, with the same peer
 * derivation: the host of the MQTT URL, on the client side FTP port.
 */
static ncl_file_tool_state *file_state_of(ncl_server *server)
{
    ncl_file_tool_state *state =
        (ncl_file_tool_state *)ncl_server_user_data(server);
    char *sn;

    if (state != NULL) {
        return state;
    }
    state = (ncl_file_tool_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    sn = ncl_sn_read();
    state->sn = sn != NULL ? sn : ncl_strdup(ncl_server_sn(server));
    {
        ncl_mqtt_config config;
        char *host = NULL;
        unsigned port = 0;
        bool tls = false;

        memset(&config, 0, sizeof(config));
        if (ncl_mqtt_config_read(&config) == NCL_OK && config.url != NULL &&
            ncl_socket_parse_url(config.url, &host, &port, &tls) == NCL_OK) {
            /* Host part of the broker URL. */
            state->remote = ncl_server_file_tool_create(
                host, NCL_FTP_CLIENT_HOLDER_PORT, NCL_FTP_DEFAULT_USER,
                NCL_FTP_DEFAULT_PASSWORD, state->sn);
        }
        free(host);
        ncl_mqtt_config_free(&config);
    }
    if (state->remote == NULL) {
        state->remote = ncl_server_file_tool_create(
            "127.0.0.1", NCL_FTP_CLIENT_HOLDER_PORT, NCL_FTP_DEFAULT_USER,
            NCL_FTP_DEFAULT_PASSWORD, state->sn);
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
    if (state == NULL || state->remote == NULL) {
        return NCL_ERR_NOMEM;
    }
    return ncl_server_register_tool(server, "file", state, k_file_methods,
                                    sizeof(k_file_methods) /
                                        sizeof(k_file_methods[0]),
                                    k_file_bindings,
                                    sizeof(k_file_bindings) /
                                        sizeof(k_file_bindings[0]));
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

/* ---------------------------------------------- process wide FTP endpoint -- */

static ncl_ftp_server *g_holder_ftp;

ncl_err ncl_client_holder_start_ftp(void)
{
    ncl_ftp_server_options options;

    if (g_holder_ftp != NULL && ncl_ftp_server_is_running(g_holder_ftp)) {
        return NCL_OK;
    }
    memset(&options, 0, sizeof(options));
    options.port = NCL_FTP_CLIENT_HOLDER_PORT;
    options.root = ncl_env_root();
    options.user = NCL_FTP_DEFAULT_USER;
    options.password = NCL_FTP_DEFAULT_PASSWORD;
    options.allow_write = true;
    g_holder_ftp = ncl_ftp_server_create_ex(&options);
    if (g_holder_ftp == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (ncl_ftp_server_start(g_holder_ftp) != NCL_OK) {
        ncl_ftp_server_free(g_holder_ftp);
        g_holder_ftp = NULL;
        return NCL_ERR_CONNECT;
    }
    ncl_log_info("FTP服务器已启动,端口:%d", NCL_FTP_CLIENT_HOLDER_PORT);
    return NCL_OK;
}

void ncl_client_holder_stop_ftp(void)
{
    if (g_holder_ftp != NULL) {
        ncl_ftp_server_free(g_holder_ftp);
        g_holder_ftp = NULL;
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

        if (value == NULL || ncl_json_type_of(value) != NCL_JSON_OBJECT ||
            key == NULL || strcmp(key, "fileKeys") == 0) {
            continue;
        }
        path = ncl_json_obj_get_string(value, NCL_FILE_MARKER);
        if (path == NULL) {
            continue;
        }
        name = remote_basename(path);
        snprintf(target, sizeof(target), "%s%ctemp%c%s", ncl_env_root(),
                 NCL_PATH_SEP, NCL_PATH_SEP, name != NULL ? name : "");
        snprintf(token, sizeof(token), "%s/%s", NCL_FILE_TEMP_DIR,
                 name != NULL ? name : "");
        if (ncl_file_copy(path, target) != NCL_OK) {
            ncl_log_error("无法复制临时文件: %s", path);
            continue;
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
