/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - client side file channel.
 *
 * The bytes of a file transfer never travel over MQTT: the device is the FTP
 * client and the controller side is the FTP server. So before the first byte
 * can move, the controller has to tell the device where to dial - that is the
 * file/openFileChannel handshake, and this file holds both halves of it: the
 * process wide FTP endpoint the controller advertises, and the two method calls
 * (openFileChannel / closeFileChannel) that put a lease on the device.
 *
 * The handshake is an extension of this library, not of GB/T 41970-2022: it
 * lives in the built in "file" tool next to write / read / ll / mkdir / delete.
 * A device that never saw a channel answers those with NCL_ERR_NO_CHANNEL.
 */
#include "file_internal.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"

/* The method names of the handshake, as the device registers them. */
#define NCL_FILE_CHANNEL_OPEN_METHOD "file/openFileChannel"
#define NCL_FILE_CHANNEL_CLOSE_METHOD "file/closeFileChannel"

/* Login the library mints for a channel. */
#define NCL_FILE_CHANNEL_USER_MAX 64
#define NCL_FILE_CHANNEL_PASSWORD_MAX 40

/* ==================================================== conf/ftp.txt ======= */

/** `<root>/conf/ftp.txt`, where the client side settings live. */
static void config_path(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s%cconf%cftp.txt", ncl_env_root(), NCL_PATH_SEP,
             NCL_PATH_SEP);
}

/** Replace *slot with a copy of @p value (NULL or empty clears it). */
static void config_replace(char **slot, const char *value)
{
    char *copy = NULL;

    if (value != NULL && value[0] != '\0') {
        copy = ncl_strdup(value);
        if (copy == NULL) {
            return; /* keep what was there: the caller sees a usable struct */
        }
    }
    ncl_mem_free(*slot);
    *slot = copy;
}

/** A JSON number as a port: negative, zero and > 65535 all mean "not given". */
static unsigned config_port(const ncl_json *json, const char *key)
{
    long long value = ncl_json_obj_get_int(json, key, 0);

    return value > 0 && value <= 65535 ? (unsigned)value : 0u;
}

void ncl_file_channel_config_free(ncl_file_channel_config *config)
{
    if (config == NULL) {
        return;
    }
    ncl_mem_free(config->root);
    ncl_mem_free(config->user);
    ncl_mem_free(config->password);
    ncl_mem_free(config->host);
    ncl_mem_free(config->path);
    memset(config, 0, sizeof(*config));
}

ncl_err ncl_file_channel_config_read(ncl_file_channel_config *out)
{
    char path[NCL_PATH_MAX_BUF];
    char *text = NULL;
    ncl_json *json = NULL;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    config_path(path, sizeof(path));
    if (ncl_path_exists(path) &&
        ncl_file_read_all(path, &text, NULL) == NCL_OK && text != NULL) {
        json = ncl_json_parse_cstr(text, NULL);
        ncl_mem_free(text);
    }
    if (json == NULL || ncl_json_type_of(json) != NCL_JSON_OBJECT) {
        if (ncl_path_exists(path)) {
            ncl_log_warn("conf/ftp.txt 不是合法 JSON 对象，客户端文件通道按默认值走");
        }
        ncl_json_free(json);
        return NCL_OK; /* the file is optional */
    }

    config_replace(&out->host, ncl_json_obj_get_string(json, "host"));
    config_replace(&out->root, ncl_json_obj_get_string(json, "root"));
    config_replace(&out->user, ncl_json_obj_get_string(json, "userName"));
    config_replace(&out->password, ncl_json_obj_get_string(json, "password"));
    config_replace(&out->path, ncl_json_obj_get_string(json, "path"));
    out->port = config_port(json, "port");
    out->advertise_port = config_port(json, "advertisePort");
    out->force = ncl_json_obj_get_bool(json, "force", false);
    out->passive = ncl_json_obj_get_bool(json, "passive", false);
    ncl_json_free(json);

    /* An account without a password is legal (empty password), not "half set". */
    if (out->user != NULL && out->password == NULL) {
        out->password = ncl_strdup("");
    }
    if (out->advertise_port == 0 && out->port != 0) {
        out->advertise_port = out->port;
    }
    return NCL_OK;
}

ncl_err ncl_file_channel_config_write(const ncl_file_channel_config *config)
{
    char path[NCL_PATH_MAX_BUF];
    char dir[NCL_PATH_MAX_BUF];
    ncl_json *json;
    char *text;
    ncl_err rc;

    if (config == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_object();
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (config->host != NULL) {
        ncl_json_obj_set_string(json, "host", config->host);
    }
    if (config->port != 0) {
        ncl_json_obj_set_int(json, "port", config->port);
    }
    if (config->advertise_port != 0) {
        ncl_json_obj_set_int(json, "advertisePort", config->advertise_port);
    }
    if (config->root != NULL) {
        ncl_json_obj_set_string(json, "root", config->root);
    }
    if (config->user != NULL) {
        ncl_json_obj_set_string(json, "userName", config->user);
    }
    if (config->password != NULL) {
        ncl_json_obj_set_string(json, "password", config->password);
    }
    if (config->path != NULL) {
        ncl_json_obj_set_string(json, "path", config->path);
    }
    if (config->force) {
        ncl_json_obj_set_bool(json, "force", true);
    }
    if (config->passive) {
        ncl_json_obj_set_bool(json, "passive", true);
    }
    text = ncl_json_write_string(json);
    ncl_json_free(json);
    if (text == NULL) {
        return NCL_ERR_NOMEM;
    }
    snprintf(dir, sizeof(dir), "%s%cconf", ncl_env_root(), NCL_PATH_SEP);
    snprintf(path, sizeof(path), "%s%cftp.txt", dir, NCL_PATH_SEP);
    ncl_mkdir_p(dir);
    rc = ncl_file_write_all(path, text, strlen(text));
    ncl_mem_free(text);
    return rc;
}

/* ===================================================== process wide FTP ==== */

/** The FTP endpoint every file channel of this process advertises. */
static ncl_ftp_server *g_holder_ftp;

ncl_err ncl_client_holder_start_ftp_ex(unsigned port, const char *root,
                                      const char *user, const char *password)
{
    ncl_file_channel_config config;
    ncl_ftp_server_options options;

    if (g_holder_ftp != NULL && ncl_ftp_server_is_running(g_holder_ftp)) {
        return NCL_OK;
    }
    /*
     * conf/ftp.txt fills whatever the caller left out (0 / NULL), so an
     * application can point the endpoint at another port, root or account
     * without touching its source. create_ex() copies the strings.
     */
    memset(&config, 0, sizeof(config));
    (void)ncl_file_channel_config_read(&config);
    memset(&options, 0, sizeof(options));
    options.port = port != 0 ? port
                             : (config.port != 0
                                    ? config.port
                                    : (unsigned)NCL_FTP_CLIENT_HOLDER_PORT);
    options.root = root != NULL && root[0] != '\0'
                       ? root
                       : (config.root != NULL ? config.root : ncl_env_root());
    options.user = user != NULL && user[0] != '\0'
                       ? user
                       : (config.user != NULL ? config.user
                                              : NCL_FTP_DEFAULT_USER);
    options.password = password != NULL && password[0] != '\0'
                           ? password
                           : (config.password != NULL
                                  ? config.password
                                  : NCL_FTP_DEFAULT_PASSWORD);
    options.allow_write = true;
    g_holder_ftp = ncl_ftp_server_create_ex(&options);
    ncl_file_channel_config_free(&config);
    if (g_holder_ftp == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (ncl_ftp_server_start(g_holder_ftp) != NCL_OK) {
        ncl_ftp_server_free(g_holder_ftp);
        g_holder_ftp = NULL;
        return NCL_ERR_CONNECT;
    }
    ncl_log_info("FTP服务器已启动,端口:%d", options.port);

    return NCL_OK;
}

ncl_err ncl_client_holder_start_ftp(void)
{
    return ncl_client_holder_start_ftp_ex(0, NULL, NULL, NULL);
}

void ncl_client_holder_stop_ftp(void)
{
    if (g_holder_ftp != NULL) {
        ncl_ftp_server_free(g_holder_ftp);
        g_holder_ftp = NULL;
    }
}

ncl_ftp_server *ncl_client_holder_ftp_endpoint(void)
{
    return g_holder_ftp != NULL && ncl_ftp_server_is_running(g_holder_ftp)
               ? g_holder_ftp
               : NULL;
}

/* ========================================================== handshake ===== */

void ncl_file_channel_options_default(ncl_file_channel_options *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
}

/** Copy the set fields of @p from over @p into. */
static void channel_options_merge(ncl_file_channel_options *into,
                                  const ncl_file_channel_options *from)
{
    if (from == NULL) {
        return;
    }
    if (from->host != NULL && from->host[0] != '\0') {
        into->host = from->host;
    }
    if (from->port != 0) {
        into->port = from->port;
    }
    if (from->user != NULL && from->user[0] != '\0') {
        into->user = from->user;
    }
    if (from->password != NULL && from->password[0] != '\0') {
        into->password = from->password;
    }
    if (from->channel_id != NULL && from->channel_id[0] != '\0') {
        into->channel_id = from->channel_id;
    }
    if (from->path != NULL && from->path[0] != '\0') {
        into->path = from->path;
    }
    if (from->force) {
        into->force = true;
    }
    if (from->timeout_ms != 0) {
        into->timeout_ms = from->timeout_ms;
    }
}

/**
 * Address a device on the same network can dial back.
 *
 * The MQTT connection is the only hint about which of our addresses the device
 * can see, so the local address of the route to the broker is the best guess.
 * It is only a guess: a broker on this very machine (or one reached over a VPN
 * interface the device cannot use) leaves an address the device cannot dial, so
 * a loopback answer falls back to the machine's first non-loopback IPv4 and
 * 127.0.0.1 is the last resort. Cross-network deployments pass options->host.
 */
static void channel_default_host(char *out, size_t out_len)
{
    ncl_mqtt_config config;
    const char *uri;
    const char *lan;
    char *host = NULL;
    unsigned port = 0;
    bool tls = false;

    out[0] = '\0';
    memset(&config, 0, sizeof(config));
    (void)ncl_mqtt_config_read(&config);
    uri = ncl_client_holder_server_uri();
    if (uri == NULL) {
        uri = config.url;
    }
    if (uri != NULL && ncl_socket_parse_url(uri, &host, &port, &tls) == NCL_OK &&
        host != NULL) {
        (void)ncl_socket_local_ip_toward(host, port, out, out_len);
    }
    ncl_mem_free(host);
    ncl_mqtt_config_free(&config);

    /* 127.x means "the broker and this process share a machine": no peer on the
     * network can dial that, hand out the LAN address instead. */
    if (out[0] == '\0' || strncmp(out, "127.", 4) == 0) {
        lan = ncl_net_local_ipv4();
        if (lan != NULL && lan[0] != '\0' && strncmp(lan, "127.", 4) != 0) {
            snprintf(out, out_len, "%s", lan);
            return;
        }
    }
    if (out[0] == '\0') {
        snprintf(out, out_len, "127.0.0.1");
    }
}

/** <sn>-<pid>: a restarted process reuses its own lease. */
static void channel_default_id(const ncl_client *client, char *out,
                               size_t out_len)
{
    const char *sn = ncl_client_sn(client);

    snprintf(out, out_len, "%s-%ld", sn != NULL && sn[0] != '\0' ? sn : "nclink",
             ncl_process_id());
}

/** Login the endpoint carries for the channel; only this client can know it. */
static void channel_account_user(const ncl_client *client, char *out,
                                 size_t out_len)
{
    const char *sn = ncl_client_sn(client);

    snprintf(out, out_len, "nclink-%ld-%.40s", ncl_process_id(),
             sn != NULL ? sn : "");
}

/**
 * The process wide endpoint, started on demand (conf/ftp.txt fills the port,
 * root and account). NULL when it cannot start; the log carries the reason.
 */
static ncl_ftp_server *channel_endpoint(const ncl_file_channel_config *config)
{
    if (g_holder_ftp != NULL && ncl_ftp_server_is_running(g_holder_ftp)) {
        return g_holder_ftp;
    }
    if (ncl_client_holder_start_ftp_ex(config != NULL ? config->port : 0,
                                       config != NULL ? config->root : NULL,
                                       config != NULL ? config->user : NULL,
                                       config != NULL ? config->password : NULL) !=
        NCL_OK) {
        ncl_log_error("文件通道需要本机的 FTP 端点，但它起不来"
                      "（端口 %u 被占？先调 ncl_client_holder_start_ftp_ex()）",
                      config != NULL && config->port != 0
                          ? config->port
                          : (unsigned)NCL_FTP_CLIENT_HOLDER_PORT);
        return NULL;
    }
    return g_holder_ftp;
}

/** Send one handshake method call; *params is consumed. */
static ncl_err channel_call(ncl_client *client, const char *method,
                            ncl_json *params, unsigned timeout_ms,
                            ncl_message **out)
{
    ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    ncl_err rc;

    if (request == NULL) {
        ncl_json_free(params);
        return NCL_ERR_NOMEM;
    }
    ncl_message_set_method(request, method);
    ncl_message_set_check(request, false);
    if (params != NULL) {
        ncl_message_set_params(request, params);
    }
    rc = ncl_client_method_call(client, request, timeout_ms, out);
    if (rc != NCL_OK) {
        return rc;
    }
    if (out == NULL || *out == NULL ||
        (*out)->type != NCL_MSG_METHOD_CALL_RESPONSE) {
        return NCL_ERR;
    }
    if (!ncl_check_is_code_ok((*out)->as.method_call_response.code)) {
        const char *reason = (*out)->as.method_call_response.reason;
        ncl_log_error("%s 被拒绝: %s", method,
                      reason != NULL && reason[0] != '\0'
                          ? reason
                          : ncl_err_name(NCL_ERR_NO_CHANNEL));
        return NCL_ERR_NO_CHANNEL;
    }
    return NCL_OK;
}

ncl_err ncl_client_open_file_channel(ncl_client *client,
                                     const ncl_file_channel_options *options)
{
    ncl_file_channel_config config;
    ncl_file_channel_options opts;
    ncl_ftp_server *endpoint = NULL;
    ncl_ftp_account account;
    char account_user[NCL_FILE_CHANNEL_USER_MAX];
    char account_password[NCL_FILE_CHANNEL_PASSWORD_MAX];
    char channel_id[NCL_FILE_CHANNEL_ID_MAX];
    char host[64];
    char requested_id[NCL_FILE_CHANNEL_ID_MAX];
    const char *user;
    const char *password;
    const char *device_id;
    const char *held_login;
    unsigned port;
    unsigned timeout;
    ncl_json *params = NULL;
    ncl_message *response = NULL;
    ncl_json *data;
    bool minted = false;
    bool reused = false;
    ncl_err rc;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /*
     * conf/ftp.txt fills what the caller left open, the caller's own options
     * win over it, and the derived defaults (broker route, endpoint port,
     * minted account) are the floor.
     */
    memset(&config, 0, sizeof(config));
    (void)ncl_file_channel_config_read(&config);
    ncl_file_channel_options_default(&opts);
    channel_options_merge(&opts, options);
    if (opts.host == NULL && config.host != NULL) {
        opts.host = config.host;
    }
    if (opts.port == 0 && config.advertise_port != 0) {
        opts.port = config.advertise_port;
    }
    if (opts.user == NULL && config.user != NULL) {
        opts.user = config.user;
        opts.password = config.password;
    }
    if (opts.path == NULL && config.path != NULL) {
        opts.path = config.path;
    }
    if (config.force) {
        opts.force = true;
    }
    if (config.passive) {
        opts.passive = true;
    }
    timeout = opts.timeout_ms != 0 ? opts.timeout_ms
                                   : NCL_CLIENT_OPERATION_TIMEOUT;
    channel_default_id(client, requested_id, sizeof(requested_id));
    snprintf(channel_id, sizeof(channel_id), "%s",
             opts.channel_id != NULL ? opts.channel_id : requested_id);
    channel_default_host(host, sizeof(host));
    if (opts.host != NULL) {
        snprintf(host, sizeof(host), "%s", opts.host);
    }

    /*
     * The local endpoint, started on demand. A caller that advertises an FTP
     * server of its own elsewhere can live without ours, but the default path
     * needs it: without a listener there is nothing to hand the device.
     */
    if (opts.user == NULL || config.user != NULL) {
        endpoint = channel_endpoint(&config);
        if (endpoint == NULL && (opts.user == NULL || opts.port == 0)) {
            ncl_file_channel_config_free(&config);
            return NCL_ERR_CONNECT;
        }
    } else {
        endpoint = g_holder_ftp;
    }
    if (endpoint != NULL && opts.port == 0) {
        opts.port = ncl_ftp_server_port(endpoint);
    }

    if (opts.user == NULL) {
        /*
         * Advertise the process wide endpoint with a credential of the
         * channel's own: revoking it later closes the device's session without
         * touching the other peers logged in to the same endpoint.
         */
        if (endpoint == NULL) {
            ncl_file_channel_config_free(&config);
            return NCL_ERR_CONNECT;
        }
        port = opts.port;
        channel_account_user(client, account_user, sizeof(account_user));
        if (ncl_uuid4(account_password, sizeof(account_password)) != NCL_OK) {
            snprintf(account_password, sizeof(account_password), "nclink-%ld",
                     ncl_process_id());
        }
        memset(&account, 0, sizeof(account));
        account.user = account_user;
        account.password = account_password;
        account.allow_write = true;
        rc = ncl_ftp_server_add_account(endpoint, &account);
        if (rc != NCL_OK) {
            ncl_log_error("文件通道的临时账号注册失败: %s", ncl_err_name(rc));
            ncl_file_channel_config_free(&config);
            return rc;
        }
        minted = true;
        user = account_user;
        password = account_password;
    } else {
        if (opts.port == 0) {
            ncl_log_error("文件通道指定了 user 但没给 port（对端 FTP 服务端的端口）");
            ncl_file_channel_config_free(&config);
            return NCL_ERR_INVALID_ARG;
        }
        port = opts.port;
        user = opts.user;
        password = opts.password != NULL ? opts.password : "";
        /*
         * Credentials the caller picked. When they are meant for the endpoint
         * of this process (the advertised port is its port) register them, so
         * the device can actually log in; a foreign FTP server already knows
         * its own accounts and is left alone.
         */
        endpoint = g_holder_ftp;
        if (endpoint != NULL && ncl_ftp_server_is_running(endpoint) &&
            ncl_ftp_server_port(endpoint) == port) {
            memset(&account, 0, sizeof(account));
            account.user = user;
            account.password = password;
            account.allow_write = true;
            rc = ncl_ftp_server_add_account(endpoint, &account);
            if (rc != NCL_OK) {
                ncl_log_error("文件通道账号注册失败: %s", ncl_err_name(rc));
                ncl_file_channel_config_free(&config);
                return rc;
            }
            minted = true;
        }
    }

    params = ncl_json_new_object();
    if (params == NULL) {
        rc = NCL_ERR_NOMEM;
        goto fail;
    }
    ncl_json_obj_set_string(params, "host", host);
    ncl_json_obj_set_int(params, "port", (long long)port);
    ncl_json_obj_set_string(params, "user", user);
    ncl_json_obj_set_string(params, "password", password);
    ncl_json_obj_set_string(params, "channelId", channel_id);
    if (opts.path != NULL) {
        ncl_json_obj_set_string(params, "path", opts.path);
    }
    if (opts.force) {
        ncl_json_obj_set_bool(params, "force", true);
    }
    if (opts.passive) {
        ncl_json_obj_set_bool(params, "passive", true);
    }
    rc = channel_call(client, NCL_FILE_CHANNEL_OPEN_METHOD, params, timeout,
                      &response);
    params = NULL; /* consumed */
    if (rc != NCL_OK) {
        goto fail;
    }

    data = response->as.method_call_response.data;
    device_id = ncl_json_obj_get_string(data, "channelId");
    reused = ncl_json_obj_get_bool(data, "reused", false);
    rc = ncl_client_set_file_channel_lease(
        client, device_id != NULL ? device_id : channel_id,
        minted ? user : NULL);
    if (rc != NCL_OK) {
        goto fail;
    }
    ncl_log_info("文件通道已打开: channelId=%s, 对端 %s:%u%s%s",
                 device_id != NULL ? device_id : channel_id, host, port,
                 minted ? ", 账号 " : "", minted ? user : "");
    if (reused) {
        ncl_log_info("文件通道沿用设备上已有的同一租约");
    }
    ncl_message_free(response);
    ncl_file_channel_config_free(&config);
    return NCL_OK;

fail:
    ncl_message_free(response);
    ncl_json_free(params);
    if (minted) {
        /* A previous channel of this client keeps its login: do not revoke it.
         * @p user may still point into @p config, so compare before freeing. */
        held_login = ncl_client_file_channel_login(client);
        if (held_login == NULL || strcmp(held_login, user) != 0) {
            ncl_ftp_server_remove_account(endpoint, user);
        }
    }
    ncl_file_channel_config_free(&config);
    return rc;
}

ncl_err ncl_client_close_file_channel(ncl_client *client)
{
    char lease[NCL_FILE_CHANNEL_ID_MAX];
    char login[NCL_FILE_CHANNEL_USER_MAX];
    const char *held_login;
    ncl_json *params;
    ncl_message *response = NULL;
    ncl_err rc;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_client_file_channel_id(client, lease, sizeof(lease))) {
        return NCL_OK; /* idempotent */
    }
    held_login = ncl_client_file_channel_login(client);
    snprintf(login, sizeof(login), "%s", held_login != NULL ? held_login : "");

    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_json_obj_set_string(params, "channelId", lease);
    rc = channel_call(client, NCL_FILE_CHANNEL_CLOSE_METHOD, params,
                      NCL_CLIENT_OPERATION_TIMEOUT, &response);
    ncl_message_free(response);

    /*
     * Drop the lease locally either way: the peer asked for the channel to go
     * away, and a stale lease would only make the next open() think it is up.
     */
    (void)ncl_client_set_file_channel_lease(client, NULL, NULL);
    if (login[0] != '\0' && g_holder_ftp != NULL) {
        /* Kicks the device's live session on our endpoint, if any. */
        if (ncl_ftp_server_remove_account(g_holder_ftp, login) == NCL_OK) {
            ncl_log_info("文件通道账号已撤销: %s", login);
        }
    }
    ncl_log_info("文件通道已关闭: channelId=%s", lease);
    return rc;
}

bool ncl_client_file_channel_is_open(ncl_client *client)
{
    return ncl_client_file_channel_id(client, NULL, 0);
}

bool ncl_client_file_channel_id(ncl_client *client, char *out, size_t out_len)
{
    const char *lease = ncl_client_file_channel_lease(client);

    if (lease == NULL) {
        return false;
    }
    if (out != NULL && out_len > 0) {
        snprintf(out, out_len, "%s", lease);
    }
    return true;
}
