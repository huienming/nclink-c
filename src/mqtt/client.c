/* NC-Link core - MQTT 5.0 client transport. */
#include "nclink/ncl_mqtt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"

/* Slot counts sized for the NC-Link workload: one client keeps a handful of
 * subscriptions alive and issues short request/response exchanges. */
#define NCL_MQTT_MAX_PENDING 64   /* outbound QoS 1/2 exchanges */
#define NCL_MQTT_MAX_SUBACKS 16   /* in flight SUBSCRIBE/UNSUBSCRIBE */
#define NCL_MQTT_MAX_INBOUND_QOS2 32
#define NCL_MQTT_MAX_SUBSCRIPTIONS 64

typedef struct {
    char *filter;
    int   qos;
} ncl_mqtt_subscription;

typedef struct {
    bool     in_use;
    uint16_t packet_id;
    int      qos;
    int      stage;      /**< 0 awaiting PUBACK/PUBREC, 1 awaiting PUBCOMP */
    bool     completed;
    uint8_t  reason_code;
} ncl_mqtt_pending_publish;

typedef struct {
    bool     in_use;
    uint16_t packet_id;
    bool     completed;
    bool     is_subscribe;
    int      granted_qos;
    uint8_t  reason_code;
} ncl_mqtt_pending_sub;

struct ncl_mqtt_client {
    /* configuration (deep copied) */
    char    *url;
    char    *host;
    unsigned port;
    bool     tls;
    char    *client_id;
    char    *username;
    char    *password;
    bool     clean_start;
    unsigned keep_alive_seconds;
    unsigned connect_timeout_ms;
    bool     automatic_reconnect;
    unsigned reconnect_delay_ms;
    unsigned reconnect_max_delay_ms;
    ncl_mqtt_connected_fn    on_connect;
    ncl_mqtt_disconnected_fn on_disconnect;
    ncl_mqtt_message_fn      on_message;
    ncl_mqtt_trace_fn        on_trace;
    void    *user;

    /* state */
    ncl_mutex   *mutex;
    ncl_cond    *cond;
    ncl_mutex   *send_mutex;
    ncl_socket  *sock;
    ncl_thread  *reader;
    bool         running;      /**< reader thread alive */
    bool         connected;    /**< broker session established */
    bool         stopping;     /**< destroy requested */
    bool         user_disconnect;
    uint16_t     next_packet_id;
    char        *last_error;

    ncl_mqtt_pending_publish pending[NCL_MQTT_MAX_PENDING];
    uint16_t     pending_count;
    ncl_mqtt_pending_sub subacks[NCL_MQTT_MAX_SUBACKS];
    uint16_t     inbound_qos2[NCL_MQTT_MAX_INBOUND_QOS2];

    ncl_mqtt_subscription subscriptions[NCL_MQTT_MAX_SUBSCRIPTIONS];
    size_t       subscription_count;
};

/* =============================================================== helpers == */

void ncl_mqtt_client_options_default(ncl_mqtt_client_options *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->url = "tcp://localhost:1883";
    options->clean_start = true;
    options->keep_alive_seconds = 60;
    options->connect_timeout_ms = 10000;
    options->automatic_reconnect = true;
    options->reconnect_delay_ms = 1000;
    options->reconnect_max_delay_ms = 30000;
}

static void ncl_mqtt_client_set_error(ncl_mqtt_client *client, const char *fmt, ...)
{
    va_list ap;
    char buffer[512];

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    ncl_mutex_lock(client->mutex);
    free(client->last_error);
    client->last_error = ncl_strdup(buffer);
    ncl_mutex_unlock(client->mutex);
}

const char *ncl_mqtt_client_last_error(ncl_mqtt_client *client)
{
    static const char none[] = "";
    const char *result;
    if (client == NULL) {
        return "client is null";
    }
    ncl_mutex_lock(client->mutex);
    result = client->last_error != NULL ? client->last_error : none;
    ncl_mutex_unlock(client->mutex);
    return result;
}

/**
 * Detach the socket so it can be shut down and freed exactly once. Callers must
 * join the reader thread before releasing the returned socket.
 */
static ncl_socket *ncl_mqtt_client_detach_socket(ncl_mqtt_client *client)
{
    ncl_socket *sock;
    ncl_mutex_lock(client->send_mutex);
    sock = client->sock;
    client->sock = NULL;
    ncl_mutex_unlock(client->send_mutex);
    return sock;
}

/**
 * Send a fully framed packet. The socket is read under the send lock, so a
 * concurrent shutdown can never free it mid-send.
 */
static ncl_err ncl_mqtt_client_send(ncl_mqtt_client *client,
                                    const ncl_buffer *packet)
{
    ncl_err rc;
    ncl_socket *sock;

    ncl_mutex_lock(client->send_mutex);
    sock = client->sock;
    rc = sock != NULL ? ncl_socket_send(sock, packet->data, packet->len)
                      : NCL_ERR_CLOSED;
    ncl_mutex_unlock(client->send_mutex);

    if (rc == NCL_ERR_CLOSED) {
        return NCL_ERR_CLOSED;
    }
    if (rc != NCL_OK) {
        ncl_mqtt_client_set_error(client, "发送 MQTT 报文失败");
    }
    return rc;
}

/** Next free packet identifier for this client. */
static uint16_t ncl_mqtt_client_next_id(ncl_mqtt_client *client)
{
    uint16_t id;
    ncl_mutex_lock(client->mutex);
    client->next_packet_id++;
    if (client->next_packet_id == 0) {
        client->next_packet_id = 1; /* 0 is not a valid packet identifier */
    }
    id = client->next_packet_id;
    ncl_mutex_unlock(client->mutex);
    return id;
}

/* ============================================== pending exchange bookkeeping */

/** Reserve a slot for an outbound QoS 1/2 publish. Caller holds no lock. */
static ncl_mqtt_pending_publish *ncl_mqtt_pending_acquire(ncl_mqtt_client *client,
                                                          uint16_t packet_id,
                                                          int qos)
{
    size_t i;
    ncl_mqtt_pending_publish *slot = NULL;

    ncl_mutex_lock(client->mutex);
    for (i = 0; i < NCL_MQTT_MAX_PENDING; i++) {
        if (!client->pending[i].in_use) {
            slot = &client->pending[i];
            slot->in_use = true;
            slot->packet_id = packet_id;
            slot->qos = qos;
            slot->stage = 0;
            slot->completed = false;
            slot->reason_code = 0;
            client->pending_count++;
            break;
        }
    }
    ncl_mutex_unlock(client->mutex);
    return slot;
}

static void ncl_mqtt_pending_release(ncl_mqtt_client *client,
                                     ncl_mqtt_pending_publish *slot)
{
    if (slot == NULL) {
        return;
    }
    ncl_mutex_lock(client->mutex);
    slot->in_use = false;
    if (client->pending_count > 0) {
        client->pending_count--;
    }
    ncl_mutex_unlock(client->mutex);
}

static ncl_mqtt_pending_publish *ncl_mqtt_pending_find(ncl_mqtt_client *client,
                                                       uint16_t packet_id)
{
    size_t i;
    for (i = 0; i < NCL_MQTT_MAX_PENDING; i++) {
        if (client->pending[i].in_use &&
            client->pending[i].packet_id == packet_id) {
            return &client->pending[i];
        }
    }
    return NULL;
}

static ncl_mqtt_pending_sub *ncl_mqtt_suback_acquire(ncl_mqtt_client *client,
                                                     uint16_t packet_id,
                                                     bool is_subscribe)
{
    size_t i;
    ncl_mqtt_pending_sub *slot = NULL;

    ncl_mutex_lock(client->mutex);
    for (i = 0; i < NCL_MQTT_MAX_SUBACKS; i++) {
        if (!client->subacks[i].in_use) {
            slot = &client->subacks[i];
            slot->in_use = true;
            slot->packet_id = packet_id;
            slot->completed = false;
            slot->is_subscribe = is_subscribe;
            slot->granted_qos = -1;
            slot->reason_code = 0;
            break;
        }
    }
    ncl_mutex_unlock(client->mutex);
    return slot;
}

static void ncl_mqtt_suback_release(ncl_mqtt_client *client,
                                    ncl_mqtt_pending_sub *slot)
{
    if (slot == NULL) {
        return;
    }
    ncl_mutex_lock(client->mutex);
    slot->in_use = false;
    ncl_mutex_unlock(client->mutex);
}

/* ============================================================ subscription = */

static bool ncl_mqtt_subscription_find(ncl_mqtt_client *client, const char *filter)
{
    size_t i;
    for (i = 0; i < client->subscription_count; i++) {
        if (strcmp(client->subscriptions[i].filter, filter) == 0) {
            return true;
        }
    }
    return false;
}

/** Remember a topic filter so it can be restored after a reconnect. */
static void ncl_mqtt_subscription_remember(ncl_mqtt_client *client,
                                           const char *filter, int qos)
{
    ncl_mutex_lock(client->mutex);
    if (!ncl_mqtt_subscription_find(client, filter) &&
        client->subscription_count < NCL_MQTT_MAX_SUBSCRIPTIONS) {
        client->subscriptions[client->subscription_count].filter = ncl_strdup(filter);
        client->subscriptions[client->subscription_count].qos = qos;
        if (client->subscriptions[client->subscription_count].filter != NULL) {
            client->subscription_count++;
        }
    }
    ncl_mutex_unlock(client->mutex);
}

static void ncl_mqtt_subscription_forget(ncl_mqtt_client *client,
                                         const char *filter)
{
    size_t i;
    ncl_mutex_lock(client->mutex);
    for (i = 0; i < client->subscription_count; i++) {
        if (strcmp(client->subscriptions[i].filter, filter) == 0) {
            free(client->subscriptions[i].filter);
            memmove(&client->subscriptions[i], &client->subscriptions[i + 1],
                    (client->subscription_count - i - 1) * sizeof(ncl_mqtt_subscription));
            client->subscription_count--;
            break;
        }
    }
    ncl_mutex_unlock(client->mutex);
}

size_t ncl_mqtt_client_subscription_count(ncl_mqtt_client *client)
{
    size_t count;
    if (client == NULL) {
        return 0;
    }
    ncl_mutex_lock(client->mutex);
    count = client->subscription_count;
    ncl_mutex_unlock(client->mutex);
    return count;
}

/* =========================================================== state helpers = */

static void ncl_mqtt_client_mark_disconnected(ncl_mqtt_client *client,
                                              uint8_t reason_code,
                                              bool will_reconnect)
{
    bool notify = false;

    ncl_mutex_lock(client->mutex);
    if (client->connected) {
        client->connected = false;
        notify = true;
    }
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);

    if (notify && client->on_disconnect != NULL) {
        client->on_disconnect(client->user, reason_code, will_reconnect);
    }
}

bool ncl_mqtt_client_is_connected(ncl_mqtt_client *client)
{
    bool connected;
    if (client == NULL) {
        return false;
    }
    ncl_mutex_lock(client->mutex);
    connected = client->connected;
    ncl_mutex_unlock(client->mutex);
    return connected;
}

bool ncl_mqtt_client_wait_connected(ncl_mqtt_client *client, unsigned timeout_ms)
{
    bool connected;
    if (client == NULL) {
        return false;
    }
    ncl_mutex_lock(client->mutex);
    if (timeout_ms > 0) {
        int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
        while (!client->connected && !client->stopping) {
            int64_t now = ncl_time_monotonic_millis();
            if (now >= deadline) {
                break;
            }
            ncl_cond_wait_timeout(client->cond, client->mutex,
                                  (unsigned)(deadline - now));
        }
    }
    connected = client->connected;
    ncl_mutex_unlock(client->mutex);
    return connected;
}

/* ============================================================ packet input = */

static bool ncl_mqtt_client_read_packet(ncl_mqtt_client *client,
                                        ncl_mqtt_packet_type *type,
                                        uint8_t *flags,
                                        unsigned char **body,
                                        size_t *body_len,
                                        unsigned first_byte_timeout_ms)
{
    unsigned char first = 0;
    unsigned char length_bytes[4];
    size_t length_used = 0;
    uint32_t remaining = 0;
    unsigned char *buffer;
    int rc;
    ncl_socket *sock = client->sock; /* stable for this call; freed only by the
                                      * owning thread after detaching */

    if (sock == NULL) {
        return false;
    }
    /* The first byte is read with a timeout so the reader can service keep
     * alive while the connection is idle. */
    rc = ncl_socket_recv(sock, &first, 1, first_byte_timeout_ms);
    if (rc == NCL_SOCKET_TIMEOUT) {
        return false; /* caller treats this as "idle", not an error */
    }
    if (rc <= 0) {
        return false;
    }

    /* Remaining length: up to four continuation bytes. */
    while (length_used < 4) {
        uint32_t value = 0;
        size_t used;
        rc = ncl_socket_recv(sock, &length_bytes[length_used], 1,
                             client->connect_timeout_ms);
        if (rc <= 0) {
            return false;
        }
        length_used++;
        used = ncl_mqtt_varint_decode(length_bytes, length_used, &value);
        if (used != 0) {
            remaining = value;
            break;
        }
    }
    if (length_used == 4 && remaining == 0) {
        /* Four bytes without a terminator: malformed. */
        uint32_t value = 0;
        if (ncl_mqtt_varint_decode(length_bytes, 4, &value) == 0) {
            ncl_mqtt_client_set_error(client, "MQTT 报文长度字段非法");
            return false;
        }
        remaining = value;
    }

    buffer = (unsigned char *)malloc(remaining > 0 ? remaining : 1);
    if (buffer == NULL) {
        ncl_mqtt_client_set_error(client, "内存不足，无法读取 MQTT 报文");
        return false;
    }
    if (remaining > 0 &&
        ncl_socket_recv_exact(sock, buffer, remaining,
                              client->connect_timeout_ms) != NCL_OK) {
        free(buffer);
        return false;
    }

    *type = (ncl_mqtt_packet_type)((first >> 4) & 0x0F);
    *flags = (uint8_t)(first & 0x0F);
    *body = buffer;
    *body_len = remaining;

    if (client->on_trace != NULL) {
        client->on_trace(client->user, true, *type, remaining + length_used + 1);
    }
    return true;
}

static void ncl_mqtt_client_handle_publish(ncl_mqtt_client *client,
                                           uint8_t flags,
                                           const unsigned char *body,
                                           size_t body_len)
{
    ncl_mqtt_publish publish;
    ncl_err rc;

    rc = ncl_mqtt_decode_publish(flags, body, body_len, &publish);
    if (rc != NCL_OK) {
        ncl_mqtt_client_set_error(client, "PUBLISH 报文解析失败");
        return;
    }

    /* Acknowledge before delivering so a slow callback cannot stall the
     * broker's retransmission timer. */
    if (publish.qos == 1) {
        ncl_buffer ack;
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBACK, publish.packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            ncl_mqtt_client_send(client, &ack);
            ncl_buffer_free(&ack);
        }
    } else if (publish.qos == 2) {
        ncl_buffer ack;
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBREC, publish.packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            ncl_mqtt_client_send(client, &ack);
            ncl_buffer_free(&ack);
        }
        /* Remember the identifier until PUBREL arrives. */
        ncl_mutex_lock(client->mutex);
        {
            size_t i;
            bool stored = false;
            for (i = 0; i < NCL_MQTT_MAX_INBOUND_QOS2; i++) {
                if (client->inbound_qos2[i] == 0) {
                    client->inbound_qos2[i] = publish.packet_id;
                    stored = true;
                    break;
                }
            }
            if (!stored) {
                ncl_log_warn("MQTT: 入站 QoS2 报文表已满，丢弃 packetId=%u",
                             publish.packet_id);
            }
        }
        ncl_mutex_unlock(client->mutex);
    }

    if (client->on_message != NULL) {
        client->on_message(client->user, &publish);
    }
    ncl_mqtt_publish_free(&publish);
}

static void ncl_mqtt_client_handle_pubrel(ncl_mqtt_client *client,
                                          const unsigned char *body, size_t len)
{
    uint16_t packet_id = 0;
    ncl_buffer ack;

    if (ncl_mqtt_decode_ack(body, len, &packet_id, NULL) != NCL_OK) {
        return;
    }
    ncl_mutex_lock(client->mutex);
    {
        size_t i;
        for (i = 0; i < NCL_MQTT_MAX_INBOUND_QOS2; i++) {
            if (client->inbound_qos2[i] == packet_id) {
                client->inbound_qos2[i] = 0;
                break;
            }
        }
    }
    ncl_mutex_unlock(client->mutex);

    if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBCOMP, packet_id,
                            NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
        ncl_mqtt_client_send(client, &ack);
        ncl_buffer_free(&ack);
    }
}

static void ncl_mqtt_client_handle_puback_or_comp(ncl_mqtt_client *client,
                                                 const unsigned char *body,
                                                 size_t len, bool complete)
{
    uint16_t packet_id = 0;
    uint8_t reason = 0;

    if (ncl_mqtt_decode_ack(body, len, &packet_id, &reason) != NCL_OK) {
        return;
    }
    ncl_mutex_lock(client->mutex);
    {
        ncl_mqtt_pending_publish *slot = ncl_mqtt_pending_find(client, packet_id);
        if (slot != NULL) {
            if (complete) {
                slot->completed = true;
                slot->reason_code = reason;
            } else {
                slot->stage = 1; /* PUBREC seen, PUBREL is on its way */
            }
        }
    }
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);
}

static void ncl_mqtt_client_handle_pubrec(ncl_mqtt_client *client,
                                          const unsigned char *body, size_t len)
{
    uint16_t packet_id = 0;
    uint8_t reason = 0;
    ncl_buffer pubrel;
    ncl_mqtt_pending_publish *slot;

    if (ncl_mqtt_decode_ack(body, len, &packet_id, &reason) != NCL_OK) {
        return;
    }
    if (reason >= 0x80) {
        /* The broker rejected the publish; finish the exchange as failed. */
        ncl_mutex_lock(client->mutex);
        slot = ncl_mqtt_pending_find(client, packet_id);
        if (slot != NULL) {
            slot->completed = true;
            slot->reason_code = reason;
        }
        ncl_cond_broadcast(client->cond);
        ncl_mutex_unlock(client->mutex);
        return;
    }

    if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBREL, packet_id,
                            NCL_MQTT_REASON_SUCCESS, &pubrel) == NCL_OK) {
        ncl_mqtt_client_send(client, &pubrel);
        ncl_buffer_free(&pubrel);
    }
    ncl_mutex_lock(client->mutex);
    slot = ncl_mqtt_pending_find(client, packet_id);
    if (slot != NULL) {
        slot->stage = 1;
    }
    ncl_mutex_unlock(client->mutex);
}

static void ncl_mqtt_client_handle_suback(ncl_mqtt_client *client,
                                          const unsigned char *body, size_t len)
{
    ncl_mqtt_suback suback;
    size_t i;

    if (ncl_mqtt_decode_suback(body, len, &suback) != NCL_OK) {
        return;
    }
    ncl_mutex_lock(client->mutex);
    for (i = 0; i < NCL_MQTT_MAX_SUBACKS; i++) {
        if (client->subacks[i].in_use &&
            client->subacks[i].packet_id == suback.packet_id) {
            client->subacks[i].completed = true;
            if (suback.reason_code_count > 0) {
                client->subacks[i].reason_code = suback.reason_codes[0];
                client->subacks[i].granted_qos = suback.reason_codes[0];
            }
            break;
        }
    }
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);
    ncl_mqtt_suback_free(&suback);
}

/* ======================================================= connect / reader == */

static ncl_err ncl_mqtt_client_open_socket(ncl_mqtt_client *client)
{
    char err[256];
    ncl_socket *sock;

    err[0] = '\0';
    sock = ncl_socket_connect(client->host, client->port,
                              client->connect_timeout_ms, err, sizeof(err));
    if (sock == NULL) {
        ncl_mqtt_client_set_error(client, "连接 MQTT 服务器失败: %s", err);
        return NCL_ERR_CONNECT;
    }
    ncl_socket_set_nodelay(sock, true);
    ncl_socket_set_keepalive(sock, true);

    ncl_mutex_lock(client->mutex);
    client->sock = sock;
    ncl_mutex_unlock(client->mutex);
    return NCL_OK;
}

static ncl_err ncl_mqtt_client_handshake(ncl_mqtt_client *client, bool reconnect)
{
    ncl_mqtt_connect_options options;
    ncl_mqtt_properties props;
    ncl_buffer packet;
    ncl_mqtt_packet_type type;
    uint8_t flags = 0;
    unsigned char *body = NULL;
    size_t body_len = 0;
    ncl_mqtt_connack connack;
    ncl_err rc;

    ncl_mqtt_properties_init(&props);
    memset(&options, 0, sizeof(options));
    options.client_id = client->client_id;
    options.username = client->username;
    options.password = client->password;
    options.clean_start = client->clean_start;
    options.keep_alive_seconds = (uint16_t)client->keep_alive_seconds;
    options.properties = &props;

    rc = ncl_mqtt_encode_connect(&options, &packet);
    ncl_mqtt_properties_free(&props);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_mqtt_client_send(client, &packet);
    ncl_buffer_free(&packet);
    if (rc != NCL_OK) {
        return rc;
    }

    if (!ncl_mqtt_client_read_packet(client, &type, &flags, &body, &body_len,
                                     client->connect_timeout_ms)) {
        ncl_mqtt_client_set_error(client, "等待 CONNACK 超时");
        return NCL_ERR_TIMEOUT;
    }
    if (type != NCL_MQTT_PKT_CONNACK) {
        ncl_mqtt_client_set_error(client, "期望 CONNACK，收到 %s",
                                  ncl_mqtt_packet_type_name(type));
        free(body);
        return NCL_ERR;
    }

    rc = ncl_mqtt_decode_connack(body, body_len, &connack);
    free(body);
    if (rc != NCL_OK) {
        ncl_mqtt_client_set_error(client, "CONNACK 报文解析失败");
        return rc;
    }
    if (connack.reason_code >= 0x80) {
        ncl_mqtt_client_set_error(client, "MQTT 服务器拒绝连接: 0x%02X",
                                  connack.reason_code);
        ncl_mqtt_connack_free(&connack);
        return NCL_ERR_CONNECT;
    }

    ncl_mutex_lock(client->mutex);
    client->connected = true;
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);

    if (client->on_connect != NULL) {
        client->on_connect(client->user, reconnect, &connack);
    }
    ncl_mqtt_connack_free(&connack);
    return NCL_OK;
}

/** Re-subscribe every remembered topic after a reconnect. */
static void ncl_mqtt_client_restore_subscriptions(ncl_mqtt_client *client)
{
    size_t i;
    size_t count;
    int granted = -1;

    ncl_mutex_lock(client->mutex);
    count = client->subscription_count;
    ncl_mutex_unlock(client->mutex);

    for (i = 0; i < count; i++) {
        char filter[256];
        int qos;
        ncl_mutex_lock(client->mutex);
        if (i >= client->subscription_count) {
            ncl_mutex_unlock(client->mutex);
            break;
        }
        snprintf(filter, sizeof(filter), "%s", client->subscriptions[i].filter);
        qos = client->subscriptions[i].qos;
        ncl_mutex_unlock(client->mutex);
        ncl_mqtt_client_subscribe(client, filter, qos, 10000, &granted);
    }
}

static ncl_err ncl_mqtt_client_do_connect(ncl_mqtt_client *client, bool reconnect)
{
    ncl_err rc;

    rc = ncl_mqtt_client_open_socket(client);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = ncl_mqtt_client_handshake(client, reconnect);
    if (rc != NCL_OK) {
        ncl_socket *dead = ncl_mqtt_client_detach_socket(client);
        if (dead != NULL) {
            ncl_socket_close(dead);
        }
        return rc;
    }

    if (reconnect) {
        ncl_mqtt_client_restore_subscriptions(client);
        ncl_log_info("MQTT 已重连: %s", client->url);
    } else {
        ncl_log_info("MQTT 已连接: %s (clientId=%s)", client->url,
                     client->client_id);
    }
    return NCL_OK;
}

/**
 * Retry the connection with exponential backoff.
 * Runs on the reader thread; returns true once a session is established.
 */
static bool ncl_mqtt_client_reconnect_with_backoff(ncl_mqtt_client *client)
{
    unsigned delay = client->reconnect_delay_ms;

    while (!client->stopping) {
        unsigned elapsed = 0;
        while (elapsed < delay && !client->stopping) {
            ncl_sleep_millis(100);
            elapsed += 100;
        }
        if (client->stopping) {
            return false;
        }
        if (ncl_mqtt_client_do_connect(client, true) == NCL_OK) {
            return true;
        }
        if (delay < client->reconnect_max_delay_ms) {
            delay *= 2;
            if (delay > client->reconnect_max_delay_ms) {
                delay = client->reconnect_max_delay_ms;
            }
        }
        ncl_log_warn("MQTT 重连失败, %u ms 后重试", delay);
    }
    return false;
}

static void ncl_mqtt_client_reader(void *arg)
{
    ncl_mqtt_client *client = (ncl_mqtt_client *)arg;

    for (;;) {
        int64_t last_send = ncl_time_monotonic_millis();
        int64_t ping_sent_at = 0;
        bool ping_outstanding = false;
        unsigned keep_alive = client->keep_alive_seconds;

        /* ---------------------------------------------------- session loop -- */
        while (!client->stopping) {
        ncl_mqtt_packet_type type;
        uint8_t flags = 0;
        unsigned char *body = NULL;
        size_t body_len = 0;

        if (!ncl_mqtt_client_read_packet(client, &type, &flags, &body, &body_len,
                                         1000)) {
            if (client->stopping) {
                break;
            }
            /* Idle tick: run keep alive and watch for a dead link. */
            {
                int64_t now = ncl_time_monotonic_millis();

                if (!ncl_mqtt_client_is_connected(client)) {
                    continue; /* the loop below drives reconnects */
                }
                if (ping_outstanding && now - ping_sent_at >
                                            (int64_t)keep_alive * 2000) {
                    ncl_mqtt_client_set_error(client, "MQTT 保活超时，连接已断开");
                    break;
                }
                if (keep_alive > 0 && !ping_outstanding &&
                    now - last_send >= (int64_t)(keep_alive * 1000u) / 2) {
                    ncl_buffer ping;
                    if (ncl_mqtt_encode_ping(false, &ping) == NCL_OK) {
                        if (ncl_mqtt_client_send(client, &ping) == NCL_OK) {
                            last_send = now;
                            ping_sent_at = now;
                            ping_outstanding = true;
                        }
                        ncl_buffer_free(&ping);
                    }
                }
            }
            continue;
        }

        switch (type) {
        case NCL_MQTT_PKT_PUBLISH:
            ncl_mqtt_client_handle_publish(client, flags, body, body_len);
            break;
        case NCL_MQTT_PKT_PUBACK:
            ncl_mqtt_client_handle_puback_or_comp(client, body, body_len, true);
            break;
        case NCL_MQTT_PKT_PUBREC:
            ncl_mqtt_client_handle_pubrec(client, body, body_len);
            break;
        case NCL_MQTT_PKT_PUBCOMP:
            ncl_mqtt_client_handle_puback_or_comp(client, body, body_len, true);
            break;
        case NCL_MQTT_PKT_PUBREL:
            ncl_mqtt_client_handle_pubrel(client, body, body_len);
            break;
        case NCL_MQTT_PKT_SUBACK:
        case NCL_MQTT_PKT_UNSUBACK:
            ncl_mqtt_client_handle_suback(client, body, body_len);
            break;
        case NCL_MQTT_PKT_PINGRESP:
            ping_outstanding = false;
            break;
        case NCL_MQTT_PKT_DISCONNECT: {
            ncl_mqtt_disconnect info;
            if (ncl_mqtt_decode_disconnect(body, body_len, &info) == NCL_OK) {
                ncl_log_warn("MQTT 服务器断开连接: 0x%02X", info.reason_code);
                ncl_mqtt_disconnect_free(&info);
            }
            break;
        }
        default:
            ncl_log_debug("MQTT: 忽略报文 %s",
                          ncl_mqtt_packet_type_name(type));
            break;
        }
        free(body);
        } /* session loop */

        /* The connection is gone. Detach first: no sender can reach the socket
         * afterwards, and this thread is not inside a receive, so it is the
         * only remaining owner and may release it. */
        {
            ncl_socket *dead = ncl_mqtt_client_detach_socket(client);
            if (dead != NULL) {
                ncl_socket_shutdown(dead);
                ncl_socket_close(dead);
            }
        }

        if (client->stopping) {
            break;
        }

        {
            bool will_reconnect =
                client->automatic_reconnect && !client->user_disconnect;
            ncl_mqtt_client_mark_disconnected(
                client, NCL_MQTT_REASON_UNSPECIFIED_ERROR, will_reconnect);
            if (!will_reconnect ||
                !ncl_mqtt_client_reconnect_with_backoff(client)) {
                break;
            }
        }
        /* Reconnected: run another session loop. */
    }

    ncl_mutex_lock(client->mutex);
    client->running = false;
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);
}

/* ================================================================= public == */

ncl_mqtt_client *ncl_mqtt_client_create(const ncl_mqtt_client_options *options)
{
    ncl_mqtt_client_options defaults;
    ncl_mqtt_client *client;
    char *host = NULL;
    unsigned port = 1883;
    bool tls = false;

    if (options == NULL) {
        ncl_mqtt_client_options_default(&defaults);
        options = &defaults;
    }
    if (options->url == NULL || options->client_id == NULL) {
        return NULL;
    }
    if (ncl_socket_parse_url(options->url, &host, &port, &tls) != NCL_OK) {
        ncl_log_error("无效的 MQTT 地址: %s", options->url);
        return NULL;
    }
    if (tls) {
        ncl_log_error("暂不支持 TLS 连接: %s", options->url);
        free(host);
        return NULL;
    }

    client = (ncl_mqtt_client *)calloc(1, sizeof(*client));
    if (client == NULL) {
        free(host);
        return NULL;
    }
    client->mutex = ncl_mutex_create();
    client->cond = ncl_cond_create();
    client->send_mutex = ncl_mutex_create();
    client->url = ncl_strdup(options->url);
    client->host = host;
    client->port = port;
    client->tls = tls;
    client->client_id = ncl_strdup(options->client_id);
    client->username = options->username != NULL ? ncl_strdup(options->username) : NULL;
    client->password = options->password != NULL ? ncl_strdup(options->password) : NULL;
    client->clean_start = options->clean_start;
    client->keep_alive_seconds = options->keep_alive_seconds;
    client->connect_timeout_ms = options->connect_timeout_ms != 0
                                     ? options->connect_timeout_ms
                                     : 10000;
    client->automatic_reconnect = options->automatic_reconnect;
    client->reconnect_delay_ms = options->reconnect_delay_ms != 0
                                     ? options->reconnect_delay_ms
                                     : 1000;
    client->reconnect_max_delay_ms = options->reconnect_max_delay_ms != 0
                                         ? options->reconnect_max_delay_ms
                                         : 30000;
    client->on_connect = options->on_connect;
    client->on_disconnect = options->on_disconnect;
    client->on_message = options->on_message;
    client->on_trace = options->on_trace;
    client->user = options->user;
    client->next_packet_id = 0;
    client->last_error = ncl_strdup("");

    if (client->mutex == NULL || client->cond == NULL ||
        client->send_mutex == NULL || client->url == NULL ||
        client->client_id == NULL || client->last_error == NULL) {
        ncl_mqtt_client_destroy(client);
        return NULL;
    }
    return client;
}

void ncl_mqtt_client_destroy(ncl_mqtt_client *client)
{
    size_t i;

    if (client == NULL) {
        return;
    }
    client->stopping = true;
    client->automatic_reconnect = false;
    if (client->cond != NULL) {
        ncl_cond_broadcast(client->cond);
    }
    /* Detach and shut the socket down so the reader's blocking receive returns,
     * then join the reader before releasing the socket for good. */
    {
        ncl_socket *dead = ncl_mqtt_client_detach_socket(client);
        if (dead != NULL) {
            ncl_socket_shutdown(dead);
        }
        if (client->reader != NULL) {
            ncl_thread_join(client->reader);
            client->reader = NULL;
        }
        if (dead != NULL) {
            ncl_socket_close(dead);
        }
    }
    for (i = 0; i < client->subscription_count; i++) {
        free(client->subscriptions[i].filter);
    }
    free(client->url);
    free(client->host);
    free(client->client_id);
    free(client->username);
    free(client->password);
    free(client->last_error);
    ncl_cond_destroy(client->cond);
    ncl_mutex_destroy(client->send_mutex);
    ncl_mutex_destroy(client->mutex);
    free(client);
}

ncl_err ncl_mqtt_client_connect(ncl_mqtt_client *client)
{
    ncl_err rc;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (client->running) {
        return NCL_OK; /* already running */
    }
    client->stopping = false;
    client->user_disconnect = false;

    /* Handshake synchronously so the caller observes connect failures, then
     * hand the socket over to the reader thread. */
    rc = ncl_mqtt_client_do_connect(client, false);
    if (rc != NCL_OK) {
        return rc;
    }

    client->running = true;
    client->reader = ncl_thread_start(ncl_mqtt_client_reader, client);
    if (client->reader == NULL) {
        ncl_mqtt_client_set_error(client, "无法创建 MQTT 接收线程");
        return NCL_ERR;
    }
    return NCL_OK;
}

ncl_err ncl_mqtt_client_disconnect(ncl_mqtt_client *client)
{
    ncl_buffer packet;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    client->user_disconnect = true;

    if (ncl_mqtt_client_is_connected(client) &&
        ncl_mqtt_encode_disconnect(NCL_MQTT_REASON_SUCCESS, NULL, &packet) == NCL_OK) {
        ncl_mqtt_client_send(client, &packet);
        ncl_buffer_free(&packet);
    }

    client->stopping = true;
    client->automatic_reconnect = false;
    ncl_mutex_lock(client->mutex);
    client->connected = false;
    ncl_cond_broadcast(client->cond);
    ncl_mutex_unlock(client->mutex);

    {
        ncl_socket *dead = ncl_mqtt_client_detach_socket(client);
        if (dead != NULL) {
            ncl_socket_shutdown(dead);
        }
        if (client->reader != NULL) {
            ncl_thread_join(client->reader);
            client->reader = NULL;
        }
        if (dead != NULL) {
            ncl_socket_close(dead);
        }
    }
    client->running = false;
    ncl_log_info("MQTT 已断开: %s", client->url);
    return NCL_OK;
}

ncl_err ncl_mqtt_client_publish(ncl_mqtt_client *client, const char *topic,
                                const void *payload, size_t payload_len, int qos,
                                const ncl_mqtt_properties *properties,
                                unsigned timeout_ms)
{
    uint16_t packet_id = 0;
    ncl_mqtt_pending_publish *slot = NULL;
    ncl_buffer packet;
    ncl_err rc;

    if (client == NULL || topic == NULL || qos < 0 || qos > 2) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_mqtt_client_is_connected(client)) {
        ncl_mqtt_client_set_error(client, "MQTT 未连接，无法发布: %s", topic);
        return NCL_ERR_CLOSED;
    }

    if (qos > 0) {
        packet_id = ncl_mqtt_client_next_id(client);
        slot = ncl_mqtt_pending_acquire(client, packet_id, qos);
        if (slot == NULL) {
            ncl_mqtt_client_set_error(client, "QoS 等待表已满，发布被拒绝");
            return NCL_ERR_STATE;
        }
    }

    rc = ncl_mqtt_encode_publish(topic, (const unsigned char *)payload,
                                 payload_len, qos, false, false, packet_id,
                                 properties, &packet);
    if (rc != NCL_OK) {
        ncl_mqtt_pending_release(client, slot);
        return rc;
    }
    rc = ncl_mqtt_client_send(client, &packet);
    ncl_buffer_free(&packet);
    if (rc != NCL_OK) {
        ncl_mqtt_pending_release(client, slot);
        return rc;
    }
    if (qos == 0) {
        return NCL_OK;
    }

    /* Wait for the broker acknowledgement. */
    ncl_mutex_lock(client->mutex);
    {
        int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
        while (!slot->completed && client->connected && !client->stopping) {
            int64_t now = ncl_time_monotonic_millis();
            if (now >= deadline) {
                break;
            }
            ncl_cond_wait_timeout(client->cond, client->mutex,
                                  (unsigned)(deadline - now));
        }
    }
    if (!slot->completed) {
        rc = client->connected ? NCL_ERR_TIMEOUT : NCL_ERR_CLOSED;
        ncl_mqtt_client_set_error(client, "等待 %s 确认超时: %s",
                                  qos == 2 ? "PUBCOMP" : "PUBACK", topic);
    } else if (slot->reason_code >= 0x80) {
        rc = NCL_ERR_IO;
        ncl_mqtt_client_set_error(client, "MQTT 服务器拒绝发布 (0x%02X): %s",
                                  slot->reason_code, topic);
    } else {
        rc = NCL_OK;
    }
    ncl_mutex_unlock(client->mutex);

    ncl_mqtt_pending_release(client, slot);
    return rc;
}

ncl_err ncl_mqtt_client_subscribe(ncl_mqtt_client *client, const char *topic_filter,
                                  int qos, unsigned timeout_ms, int *granted_qos)
{
    uint16_t packet_id;
    ncl_mqtt_pending_sub *slot;
    ncl_buffer packet;
    ncl_err rc;
    bool completed = false;
    int granted = -1;
    uint8_t reason = 0;

    if (client == NULL || topic_filter == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (granted_qos != NULL) {
        *granted_qos = -1;
    }

    /* Remember the filter even while offline so it survives a reconnect. */
    ncl_mqtt_subscription_remember(client, topic_filter, qos);

    if (!ncl_mqtt_client_is_connected(client)) {
        return NCL_ERR_CLOSED;
    }

    packet_id = ncl_mqtt_client_next_id(client);
    slot = ncl_mqtt_suback_acquire(client, packet_id, true);
    if (slot == NULL) {
        return NCL_ERR_STATE;
    }

    rc = ncl_mqtt_encode_subscribe(packet_id, topic_filter, qos, NULL, &packet);
    if (rc == NCL_OK) {
        rc = ncl_mqtt_client_send(client, &packet);
    }
    ncl_buffer_free(&packet);
    if (rc != NCL_OK) {
        ncl_mqtt_suback_release(client, slot);
        return rc;
    }

    ncl_mutex_lock(client->mutex);
    {
        int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
        while (!slot->completed && client->connected && !client->stopping) {
            int64_t now = ncl_time_monotonic_millis();
            if (now >= deadline) {
                break;
            }
            ncl_cond_wait_timeout(client->cond, client->mutex,
                                  (unsigned)(deadline - now));
        }
        completed = slot->completed;
        granted = slot->granted_qos;
        reason = slot->reason_code;
    }
    ncl_mutex_unlock(client->mutex);
    ncl_mqtt_suback_release(client, slot);

    if (!completed) {
        ncl_mqtt_client_set_error(client, "等待 SUBACK 超时: %s", topic_filter);
        return client->connected ? NCL_ERR_TIMEOUT : NCL_ERR_CLOSED;
    }
    if (reason >= 0x80) {
        ncl_mqtt_client_set_error(client, "订阅被拒绝 (0x%02X): %s", reason,
                                  topic_filter);
        return NCL_ERR_IO;
    }
    if (granted_qos != NULL) {
        *granted_qos = granted;
    }
    return NCL_OK;
}

ncl_err ncl_mqtt_client_unsubscribe(ncl_mqtt_client *client,
                                    const char *topic_filter, unsigned timeout_ms)
{
    uint16_t packet_id;
    ncl_mqtt_pending_sub *slot;
    ncl_buffer packet;
    ncl_err rc;
    bool completed = false;

    if (client == NULL || topic_filter == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mqtt_subscription_forget(client, topic_filter);

    if (!ncl_mqtt_client_is_connected(client)) {
        return NCL_ERR_CLOSED;
    }

    packet_id = ncl_mqtt_client_next_id(client);
    slot = ncl_mqtt_suback_acquire(client, packet_id, false);
    if (slot == NULL) {
        return NCL_ERR_STATE;
    }

    rc = ncl_mqtt_encode_unsubscribe(packet_id, topic_filter, NULL, &packet);
    if (rc == NCL_OK) {
        rc = ncl_mqtt_client_send(client, &packet);
    }
    ncl_buffer_free(&packet);
    if (rc != NCL_OK) {
        ncl_mqtt_suback_release(client, slot);
        return rc;
    }

    ncl_mutex_lock(client->mutex);
    {
        int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
        while (!slot->completed && client->connected && !client->stopping) {
            int64_t now = ncl_time_monotonic_millis();
            if (now >= deadline) {
                break;
            }
            ncl_cond_wait_timeout(client->cond, client->mutex,
                                  (unsigned)(deadline - now));
        }
        completed = slot->completed;
    }
    ncl_mutex_unlock(client->mutex);
    ncl_mqtt_suback_release(client, slot);

    if (!completed) {
        ncl_mqtt_client_set_error(client, "等待 UNSUBACK 超时: %s", topic_filter);
        return client->connected ? NCL_ERR_TIMEOUT : NCL_ERR_CLOSED;
    }
    return NCL_OK;
}
