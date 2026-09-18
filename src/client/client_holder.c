/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - process wide client manager.
 *
 * Owns the process wide MQTT connection, implements the message channel interface
 * on top of it, and keeps one ncl_client per device serial number with a 30 minute
 * idle expiry; an expired client is unsubscribed before it is released.
 *
 * Subscribing happens once, when a client is created: re-subscribing on every
 * lookup would re-send the SUBSCRIBE packets for every inbound message.
 */
#include "nclink/ncl_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_socket.h"
#include "nclink/ncl_thread.h"
#include "nclink/ncl_topic.h"
#include "nclink/ncl_file.h"

typedef struct {
    char                 *sn;
    ncl_client           *client;
    ncl_file_client_tool *file_tool; /**< owned; freed before the client */
    int64_t               last_access_ms;
} ncl_client_slot;

typedef struct {
    ncl_message_channel channel; /**< must be first: implements the interface */

    ncl_mutex       *mutex;
    ncl_mqtt_client *mqtt;
    bool             initialised;
    char            *server_uri; /**< broker URL this client was built with */

    ncl_client_slot *slots;
    size_t           slot_count;
    size_t           slot_capacity;
} ncl_client_holder;

static ncl_client_holder *g_holder = NULL;
static ncl_mutex         *g_holder_mutex = NULL;

/* User property every NC-Link publish carries. */
#define NCL_PROTOCOL_VERSION_PROPERTY "version"
#define NCL_PROTOCOL_VERSION_VALUE "2.0"

/* ============================================================ MQTT channel == */

static ncl_err ncl_holder_publish(ncl_message_channel *self, const char *topic,
                                  const ncl_message *message, int qos,
                                  const ncl_mqtt_properties *properties)
{
    ncl_client_holder *holder = (ncl_client_holder *)self;
    ncl_mqtt_properties props;
    char *payload;
    ncl_err rc;

    if (holder->mqtt == NULL) {
        return NCL_ERR_CLOSED;
    }
    payload = ncl_message_write_string(message);
    if (payload == NULL) {
        return NCL_ERR_NOMEM;
    }

    /* The marker is attached when the caller passes no properties. */
    if (properties == NULL) {
        ncl_mqtt_properties_init(&props);
        ncl_mqtt_properties_add_user(&props, NCL_PROTOCOL_VERSION_PROPERTY,
                                     NCL_PROTOCOL_VERSION_VALUE);
    }

    rc = ncl_mqtt_client_publish(holder->mqtt, topic, payload, strlen(payload),
                                 qos, properties != NULL ? properties : &props,
                                 NCL_CLIENT_OPERATION_TIMEOUT * 4);
    if (properties == NULL) {
        ncl_mqtt_properties_free(&props);
    }
    if (rc != NCL_OK) {
        ncl_log_error("MQTT 发布失败: topic=%s, %s", topic,
                      ncl_mqtt_client_last_error(holder->mqtt));
    }
    ncl_mem_free(payload);
    return rc;
}

static ncl_err ncl_holder_subscribe(ncl_message_channel *self, const char *topic,
                                    int qos)
{
    ncl_client_holder *holder = (ncl_client_holder *)self;
    int granted = -1;
    ncl_err rc;

    if (holder->mqtt == NULL) {
        return NCL_ERR_CLOSED;
    }
    rc = ncl_mqtt_client_subscribe(holder->mqtt, topic, qos, 10000, &granted);
    if (rc != NCL_OK) {
        ncl_log_error("订阅失败: topic=%s, %s", topic,
                      ncl_mqtt_client_last_error(holder->mqtt));
    }
    return rc;
}

static ncl_err ncl_holder_unsubscribe(ncl_message_channel *self, const char *topic)
{
    ncl_client_holder *holder = (ncl_client_holder *)self;

    if (holder->mqtt == NULL) {
        return NCL_ERR_CLOSED;
    }
    return ncl_mqtt_client_unsubscribe(holder->mqtt, topic, 10000);
}

/* ============================================================ client slots == */

/** Drop clients idle for longer than NCL_CLIENT_IDLE_TTL_MS: unsubscribe,
 *  then release. Caller holds the lock. */
static void ncl_holder_sweep_idle(ncl_client_holder *holder)
{
    int64_t now = ncl_time_monotonic_millis();
    size_t i = 0;

    while (i < holder->slot_count) {
        ncl_client_slot *slot = &holder->slots[i];
        if (now - slot->last_access_ms > NCL_CLIENT_IDLE_TTL_MS) {
            ncl_log_info("客户端空闲超时，已释放: sn=%s", slot->sn);
            ncl_client_unsubscribe(slot->client);
            ncl_client_set_file_tool(slot->client, NULL);
            ncl_file_client_tool_free(slot->file_tool);
            ncl_client_free(slot->client);
            ncl_mem_free(slot->sn);
            memmove(&holder->slots[i], &holder->slots[i + 1],
                    (holder->slot_count - i - 1) * sizeof(ncl_client_slot));
            holder->slot_count--;
            continue;
        }
        i++;
    }
}

static ncl_client_slot *ncl_holder_find_slot(ncl_client_holder *holder,
                                             const char *sn)
{
    size_t i;
    for (i = 0; i < holder->slot_count; i++) {
        if (strcmp(holder->slots[i].sn, sn) == 0) {
            return &holder->slots[i];
        }
    }
    return NULL;
}

/** Get or create the client for @p sn. Caller holds the lock. */
static ncl_client *ncl_holder_get_locked(ncl_client_holder *holder, const char *sn)
{
    ncl_client_slot *slot;

    ncl_holder_sweep_idle(holder);
    slot = ncl_holder_find_slot(holder, sn);
    if (slot == NULL) {
        ncl_client *client = ncl_client_create(sn, &holder->channel);
        if (client == NULL) {
            return NULL;
        }
        if (holder->slot_count == holder->slot_capacity) {
            size_t capacity = holder->slot_capacity == 0 ? 8
                                                         : holder->slot_capacity * 2;
            ncl_client_slot *grown = (ncl_client_slot *)ncl_mem_realloc(
                holder->slots, capacity * sizeof(ncl_client_slot));
            if (grown == NULL) {
                ncl_client_free(client);
                return NULL;
            }
            holder->slots = grown;
            holder->slot_capacity = capacity;
        }
        slot = &holder->slots[holder->slot_count];
        slot->sn = ncl_strdup(sn);
        slot->client = client;
        /* Install the file channel and create <cwd>/<sn>. */
        slot->file_tool = ncl_file_client_tool_create(client);
        if (slot->file_tool != NULL) {
            ncl_file_client_tool_detect(slot->file_tool);
            ncl_client_set_file_tool(client, slot->file_tool);
        }
        slot->last_access_ms = ncl_time_monotonic_millis();
        if (slot->sn == NULL) {
            ncl_client_set_file_tool(client, NULL);
            ncl_file_client_tool_free(slot->file_tool);
            ncl_client_free(client);
            return NULL;
        }
        holder->slot_count++;
        /* Subscribe once, as soon as the client exists. */
        ncl_client_subscribe(client);
        ncl_log_info("已创建设备客户端: sn=%s", sn);
    } else {
        slot->last_access_ms = ncl_time_monotonic_millis();
    }
    return slot->client;
}

/* ========================================================= inbound routing = */

static void ncl_holder_on_message(void *user, const ncl_mqtt_publish *publish)
{
    ncl_client_holder *holder = (ncl_client_holder *)user;
    char topic[512];
    char *sn;
    ncl_client *client;
    ncl_message *message;

    if (publish->topic == NULL || publish->payload_len == 0) {
        return;
    }
    snprintf(topic, sizeof(topic), "%s", publish->topic);

    /* Serial number of the peer this topic addresses. */
    sn = ncl_topic_extract_sn(topic);
    if (sn == NULL) {
        ncl_log_warn("无法解析主题中的序列号: %s", topic);
        return;
    }

    message = ncl_message_parse(topic, (const char *)publish->payload,
                                publish->payload_len);
    if (message == NULL) {
        ncl_mem_free(sn);
        return;
    }

    ncl_mutex_lock(holder->mutex);
    client = ncl_holder_get_locked(holder, sn);
    ncl_mutex_unlock(holder->mutex);
    ncl_mem_free(sn);

    if (client == NULL) {
        ncl_message_free(message);
        return;
    }
    ncl_client_on_message(client, topic, message);
}

static void ncl_holder_on_disconnect(void *user, uint8_t reason_code,
                                     bool will_reconnect)
{
    (void)user;
    ncl_log_warn("MQTT 连接断开: reason=0x%02X, 将重连=%s", reason_code,
                 will_reconnect ? "是" : "否");
}

/* ============================================================== public API == */

void ncl_client_holder_options_default(ncl_client_holder_options *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->tls_verify_peer = true;
    /* 用 _default() 拿到的结构体就是"每个字段都显式给了值"（关校验要自己改成 false）。 */
    options->tls_verify_peer_set = true;
}

ncl_err ncl_client_holder_init_ex(const ncl_client_holder_options *options_in)
{
    ncl_mqtt_client_options options;
    ncl_client_holder *holder;
    char client_id[37];
    const char *server_uri =
        options_in != NULL ? options_in->server_uri : NULL;
    const char *username = options_in != NULL ? options_in->username : NULL;
    const char *password = options_in != NULL ? options_in->password : NULL;

    if (server_uri == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (g_holder_mutex == NULL) {
        g_holder_mutex = ncl_mutex_create();
        if (g_holder_mutex == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    ncl_mutex_lock(g_holder_mutex);
    if (g_holder != NULL) {
        ncl_mutex_unlock(g_holder_mutex);
        return NCL_OK; /* already initialised */
    }

    /*
     * ssl:// 需要带 TLS 编译的库：这里先挡住并给出明确的错误码，别让调用方在
     * "连接失败"里猜（NCL_ERR_NOT_SUPPORTED = 功能没编进来）。
     */
    {
        char *tls_host = NULL;
        unsigned tls_port = 0;
        bool tls_url = false;

        if (ncl_socket_parse_url(server_uri, &tls_host, &tls_port, &tls_url) ==
                NCL_OK &&
            tls_url && !ncl_socket_tls_available()) {
            ncl_mem_free(tls_host);
            ncl_log_error("TLS 未编译进本库（用 -DNCLINK_WITH_TLS=ON 重新构建）: %s",
                          server_uri);
            ncl_mutex_unlock(g_holder_mutex);
            return NCL_ERR_NOT_SUPPORTED;
        }
        ncl_mem_free(tls_host);
    }

    holder = (ncl_client_holder *)ncl_mem_calloc(1, sizeof(ncl_client_holder));
    if (holder == NULL) {
        ncl_mutex_unlock(g_holder_mutex);
        return NCL_ERR_NOMEM;
    }
    holder->mutex = ncl_mutex_create();
    holder->channel.publish = ncl_holder_publish;
    holder->channel.subscribe = ncl_holder_subscribe;
    holder->channel.unsubscribe = ncl_holder_unsubscribe;
    holder->server_uri = ncl_strdup(server_uri);

    /* Client identifier: a random UUID per connection. */
    if (ncl_uuid4(client_id, sizeof(client_id)) != NCL_OK) {
        snprintf(client_id, sizeof(client_id), "nclink-c-%ld", ncl_process_id());
    }
    ncl_log_info("客户端连接: %s", client_id);

    ncl_mqtt_client_options_default(&options);
    options.url = server_uri;
    options.client_id = client_id;
    options.username = username;
    options.password = password;
    options.clean_start = true;         /* always a clean session                */
    options.keep_alive_seconds = 60;    /* keep alive: 60 s                       */
    options.connect_timeout_ms = 10000; /* connect timeout: 10 s                  */
    options.automatic_reconnect = true; /* re-establish dropped connections       */
    options.on_message = ncl_holder_on_message;
    options.on_disconnect = ncl_holder_on_disconnect;
    options.user = holder;
    if (options_in != NULL) {
        /* TLS 选项原样带下去（NULL / 默认值等于"不覆盖"）。 */
        options.tls_ca_file = options_in->tls_ca_file;
        options.tls_client_cert = options_in->tls_client_cert;
        options.tls_client_key = options_in->tls_client_key;
        options.tls_server_name = options_in->tls_server_name;
        if (options_in->tls_verify_peer_set) {
            options.tls_verify_peer = options_in->tls_verify_peer;
        }
    }

    holder->mqtt = ncl_mqtt_client_create(&options);
    if (holder->mqtt == NULL || holder->mutex == NULL ||
        holder->server_uri == NULL) {
        ncl_mqtt_client_destroy(holder->mqtt);
        ncl_mutex_destroy(holder->mutex);
        ncl_mem_free(holder->server_uri);
        ncl_mem_free(holder);
        ncl_mutex_unlock(g_holder_mutex);
        return NCL_ERR;
    }

    {
        ncl_err rc = ncl_mqtt_client_connect(holder->mqtt);
        if (rc != NCL_OK) {
            ncl_log_error("MQTT 连接失败: %s",
                          ncl_mqtt_client_last_error(holder->mqtt));
            ncl_mqtt_client_destroy(holder->mqtt);
            ncl_mutex_destroy(holder->mutex);
            ncl_mem_free(holder->server_uri);
            ncl_mem_free(holder);
            ncl_mutex_unlock(g_holder_mutex);
            return rc;
        }
    }

    holder->initialised = true;
    g_holder = holder;
    ncl_mutex_unlock(g_holder_mutex);
    /*
     * No FTP endpoint is started here: bulk transfer now begins with
     * ncl_client_open_file_channel(), which starts the listener on demand (or
     * reuses the one started by ncl_client_holder_start_ftp*()).
     */
    return NCL_OK;
}

ncl_err ncl_client_holder_init(const char *server_uri, const char *username,
                               const char *password)
{
    ncl_client_holder_options options;

    memset(&options, 0, sizeof(options));
    options.server_uri = server_uri;
    options.username = username;
    options.password = password;
    return ncl_client_holder_init_ex(&options);
}

ncl_client *ncl_client_holder_get(const char *sn)
{
    ncl_client *client = NULL;

    if (sn == NULL || g_holder_mutex == NULL) {
        ncl_log_error("客户端管理器尚未初始化");
        return NULL;
    }
    ncl_mutex_lock(g_holder_mutex);
    if (g_holder != NULL) {
        ncl_mutex_lock(g_holder->mutex);
        client = ncl_holder_get_locked(g_holder, sn);
        ncl_mutex_unlock(g_holder->mutex);
    }
    ncl_mutex_unlock(g_holder_mutex);
    return client;
}

bool ncl_client_holder_is_initialised(void)
{
    bool ready = false;
    if (g_holder_mutex == NULL) {
        return false;
    }
    ncl_mutex_lock(g_holder_mutex);
    ready = g_holder != NULL && g_holder->initialised;
    ncl_mutex_unlock(g_holder_mutex);
    return ready;
}

ncl_mqtt_client *ncl_client_holder_mqtt(void)
{
    ncl_mqtt_client *mqtt = NULL;
    if (g_holder_mutex == NULL) {
        return NULL;
    }
    ncl_mutex_lock(g_holder_mutex);
    if (g_holder != NULL) {
        mqtt = g_holder->mqtt;
    }
    ncl_mutex_unlock(g_holder_mutex);
    return mqtt;
}

const char *ncl_client_holder_server_uri(void)
{
    const char *uri = NULL;

    if (g_holder_mutex == NULL) {
        return NULL;
    }
    ncl_mutex_lock(g_holder_mutex);
    if (g_holder != NULL) {
        uri = g_holder->server_uri;
    }
    ncl_mutex_unlock(g_holder_mutex);
    return uri;
}

size_t ncl_client_holder_client_count(void)
{
    size_t count = 0;
    if (g_holder_mutex == NULL) {
        return 0;
    }
    ncl_mutex_lock(g_holder_mutex);
    if (g_holder != NULL) {
        ncl_mutex_lock(g_holder->mutex);
        ncl_holder_sweep_idle(g_holder);
        count = g_holder->slot_count;
        ncl_mutex_unlock(g_holder->mutex);
    }
    ncl_mutex_unlock(g_holder_mutex);
    return count;
}

void ncl_client_holder_shutdown(void)
{
    size_t i;

    /* Shutdown drops the FTP endpoint alongside the MQTT client. */
    ncl_client_holder_stop_ftp();
    if (g_holder_mutex == NULL) {
        return;
    }
    ncl_mutex_lock(g_holder_mutex);
    if (g_holder != NULL) {
        ncl_client_holder *holder = g_holder;
        g_holder = NULL;

        /* Release the per-device clients first: ncl_client_free() unsubscribes
         * through the channel, so the MQTT client must still be alive. */
        ncl_mutex_lock(holder->mutex);
        for (i = 0; i < holder->slot_count; i++) {
            ncl_client_set_file_tool(holder->slots[i].client, NULL);
            ncl_file_client_tool_free(holder->slots[i].file_tool);
            ncl_client_free(holder->slots[i].client);
            ncl_mem_free(holder->slots[i].sn);
        }
        ncl_mem_free(holder->slots);
        holder->slots = NULL;
        holder->slot_count = 0;
        holder->slot_capacity = 0;
        ncl_mutex_unlock(holder->mutex);

        if (holder->mqtt != NULL) {
            ncl_mqtt_client_disconnect(holder->mqtt);
            ncl_mqtt_client_destroy(holder->mqtt);
            holder->mqtt = NULL; /* channel calls become no-ops */
        }
        ncl_mem_free(holder->server_uri);
        holder->server_uri = NULL;
        ncl_mutex_destroy(holder->mutex);
        ncl_mem_free(holder);
        ncl_log_info("客户端管理器已关闭");
    }
    ncl_mutex_unlock(g_holder_mutex);
}
