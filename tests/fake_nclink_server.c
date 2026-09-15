/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

#include "fake_nclink_server.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_thread.h"

/** History depth for ncl_fake_server_publish_at(). */
#define NCL_FAKE_HISTORY 8
/** Largest payload the test double records. The Pong reply carries the whole
 *  OpenAPI document, so this has to be comfortably larger than a few KB. */
#define NCL_FAKE_PAYLOAD_MAX 32768

typedef struct {
    char topic[512];
    char payload[NCL_FAKE_PAYLOAD_MAX];
} ncl_fake_publish_record;

struct ncl_fake_server {
    ncl_socket  *listener;
    ncl_socket  *client;   /**< current session, guarded by mutex */
    unsigned     port;
    ncl_thread  *thread;
    ncl_mutex   *mutex;
    ncl_cond    *cond;
    volatile bool stop;
    ncl_fake_request_fn on_request;
    void        *user;

    int    connections;
    size_t requests;
    int    subscribes;
    int    publish_count[3];
    char   last_topic[512];
    char   last_payload[NCL_FAKE_PAYLOAD_MAX];
    ncl_fake_publish_record history[NCL_FAKE_HISTORY];
    size_t history_count;   /**< total recorded (may exceed the ring size) */
};

static bool fake_read_packet(ncl_socket *sock, unsigned timeout_ms,
                             ncl_mqtt_packet_type *type, uint8_t *flags,
                             unsigned char **body, size_t *body_len)
{
    unsigned char first = 0;
    unsigned char length_bytes[4];
    size_t length_used = 0;
    uint32_t remaining = 0;
    unsigned char *buffer;
    int rc;

    rc = ncl_socket_recv(sock, &first, 1, timeout_ms);
    if (rc <= 0) {
        return false;
    }
    while (length_used < 4) {
        uint32_t value = 0;
        rc = ncl_socket_recv(sock, &length_bytes[length_used], 1, 2000);
        if (rc <= 0) {
            return false;
        }
        length_used++;
        if (ncl_mqtt_varint_decode(length_bytes, length_used, &value) != 0) {
            remaining = value;
            break;
        }
    }
    buffer = (unsigned char *)malloc(remaining > 0 ? remaining : 1);
    if (buffer == NULL) {
        return false;
    }
    if (remaining > 0 &&
        ncl_socket_recv_exact(sock, buffer, remaining, 5000) != NCL_OK) {
        free(buffer);
        return false;
    }
    *type = (ncl_mqtt_packet_type)((first >> 4) & 0x0F);
    *flags = (uint8_t)(first & 0x0F);
    *body = buffer;
    *body_len = remaining;
    return true;
}

static void fake_send(ncl_fake_server *server, const void *data, size_t len)
{
    ncl_mutex_lock(server->mutex);
    if (server->client != NULL) {
        ncl_socket_send(server->client, data, len);
    }
    ncl_mutex_unlock(server->mutex);
}

ncl_err ncl_fake_server_publish(ncl_fake_server *server, const char *topic,
                                const char *payload, int qos)
{
    ncl_buffer packet;
    ncl_mqtt_properties props;
    ncl_err rc;

    if (server == NULL || topic == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mqtt_properties_init(&props);
    ncl_mqtt_properties_add_user(&props, "version", "2.0");
    rc = ncl_mqtt_encode_publish(topic, (const unsigned char *)payload,
                                 payload != NULL ? strlen(payload) : 0, qos,
                                 false, false, 1, &props, &packet);
    ncl_mqtt_properties_free(&props);
    if (rc == NCL_OK) {
        fake_send(server, packet.data, packet.len);
        ncl_buffer_free(&packet);
    }
    return rc;
}

static void fake_handle_publish(ncl_fake_server *server, uint8_t flags,
                                const unsigned char *body, size_t len)
{
    ncl_mqtt_publish publish;
    static char payload[NCL_FAKE_PAYLOAD_MAX];
    size_t copy_len;

    if (ncl_mqtt_decode_publish(flags, body, len, &publish) != NCL_OK) {
        return;
    }
    ncl_mutex_lock(server->mutex);
    if (publish.qos >= 0 && publish.qos <= 2) {
        server->publish_count[publish.qos]++;
    }
    server->requests++;
    snprintf(server->last_topic, sizeof(server->last_topic), "%s", publish.topic);
    copy_len = publish.payload_len < sizeof(payload) - 1 ? publish.payload_len
                                                         : sizeof(payload) - 1;
    memcpy(payload, publish.payload, copy_len);
    payload[copy_len] = '\0';
    snprintf(server->last_payload, sizeof(server->last_payload), "%s", payload);
    {
        size_t slot = server->history_count % NCL_FAKE_HISTORY;
        snprintf(server->history[slot].topic, sizeof(server->history[slot].topic),
                 "%s", publish.topic);
        snprintf(server->history[slot].payload,
                 sizeof(server->history[slot].payload), "%s", payload);
        server->history_count++;
    }
    ncl_cond_broadcast(server->cond);
    ncl_mutex_unlock(server->mutex);

    /* Acknowledge before handing the request to the test. */
    if (publish.qos == 1) {
        ncl_buffer ack;
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBACK, publish.packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            fake_send(server, ack.data, ack.len);
            ncl_buffer_free(&ack);
        }
    } else if (publish.qos == 2) {
        ncl_buffer ack;
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBREC, publish.packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            fake_send(server, ack.data, ack.len);
            ncl_buffer_free(&ack);
        }
    }

    if (server->on_request != NULL) {
        server->on_request(server, server->user, publish.topic, payload,
                           copy_len);
    }
    ncl_mqtt_publish_free(&publish);
}

static void fake_handle_packet(ncl_fake_server *server,
                               ncl_mqtt_packet_type type, uint8_t flags,
                               const unsigned char *body, size_t len)
{
    switch (type) {
    case NCL_MQTT_PKT_CONNECT: {
        unsigned char connack[5] = {0x20, 0x03, 0x00, 0x00, 0x00};
        ncl_mutex_lock(server->mutex);
        server->connections++;
        ncl_cond_broadcast(server->cond);
        ncl_mutex_unlock(server->mutex);
        fake_send(server, connack, sizeof(connack));
        break;
    }
    case NCL_MQTT_PKT_SUBSCRIBE: {
        /* Reply with the requested QoS for every filter in the packet. */
        uint16_t packet_id = (uint16_t)((body[0] << 8) | body[1]);
        size_t pos = 3; /* id + property length */
        unsigned char suback[8];
        unsigned char codes[4];
        size_t count = 0;
        size_t i = 0;

        while (pos + 3 <= len && count < 4) {
            uint16_t topic_len = (uint16_t)((body[pos] << 8) | body[pos + 1]);
            if (pos + 2 + topic_len + 1 > len) {
                break;
            }
            codes[count] = body[pos + 2 + topic_len] & 0x03;
            count++;
            pos += 2 + topic_len + 1;
        }
        suback[0] = 0x90;
        suback[1] = (unsigned char)(2 + 1 + count);
        suback[2] = (unsigned char)(packet_id >> 8);
        suback[3] = (unsigned char)(packet_id & 0xFF);
        suback[4] = 0x00; /* empty properties */
        for (i = 0; i < count; i++) {
            suback[5 + i] = codes[i];
        }
        fake_send(server, suback, 5 + count);

        ncl_mutex_lock(server->mutex);
        server->subscribes++;
        ncl_cond_broadcast(server->cond);
        ncl_mutex_unlock(server->mutex);
        break;
    }
    case NCL_MQTT_PKT_UNSUBSCRIBE: {
        uint16_t packet_id = (uint16_t)((body[0] << 8) | body[1]);
        unsigned char unsuback[6];
        unsuback[0] = 0xB0;
        unsuback[1] = 0x04;
        unsuback[2] = (unsigned char)(packet_id >> 8);
        unsuback[3] = (unsigned char)(packet_id & 0xFF);
        unsuback[4] = 0x00;
        unsuback[5] = 0x00;
        fake_send(server, unsuback, sizeof(unsuback));
        break;
    }
    case NCL_MQTT_PKT_PUBLISH:
        fake_handle_publish(server, flags, body, len);
        break;
    case NCL_MQTT_PKT_PUBREL: {
        uint16_t packet_id = 0;
        ncl_buffer ack;
        ncl_mqtt_decode_ack(body, len, &packet_id, NULL);
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBCOMP, packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            fake_send(server, ack.data, ack.len);
            ncl_buffer_free(&ack);
        }
        break;
    }
    case NCL_MQTT_PKT_PINGREQ: {
        ncl_buffer resp;
        if (ncl_mqtt_encode_ping(true, &resp) == NCL_OK) {
            fake_send(server, resp.data, resp.len);
            ncl_buffer_free(&resp);
        }
        break;
    }
    case NCL_MQTT_PKT_DISCONNECT:
        ncl_mutex_lock(server->mutex);
        ncl_cond_broadcast(server->cond);
        ncl_mutex_unlock(server->mutex);
        break;
    default:
        break;
    }
}

static void fake_server_thread(void *arg)
{
    ncl_fake_server *server = (ncl_fake_server *)arg;

    while (!server->stop) {
        ncl_socket *client = ncl_socket_accept(server->listener, 200);
        if (client == NULL) {
            continue;
        }
        ncl_mutex_lock(server->mutex);
        server->client = client;
        ncl_mutex_unlock(server->mutex);

        while (!server->stop) {
            ncl_mqtt_packet_type type;
            uint8_t flags = 0;
            unsigned char *body = NULL;
            size_t body_len = 0;

            if (!fake_read_packet(client, 200, &type, &flags, &body, &body_len)) {
                continue; /* idle */
            }
            fake_handle_packet(server, type, flags, body, body_len);
            free(body);
            if (type == NCL_MQTT_PKT_DISCONNECT) {
                break;
            }
        }

        ncl_mutex_lock(server->mutex);
        if (server->client == client) {
            server->client = NULL;
        }
        ncl_mutex_unlock(server->mutex);
        ncl_socket_close(client);
    }
}

ncl_fake_server *ncl_fake_server_start(const ncl_fake_server_options *options)
{
    ncl_fake_server *server;
    char err[128];

    server = (ncl_fake_server *)calloc(1, sizeof(ncl_fake_server));
    if (server == NULL) {
        return NULL;
    }
    server->mutex = ncl_mutex_create();
    server->cond = ncl_cond_create();
    if (options != NULL) {
        server->on_request = options->on_request;
        server->user = options->user;
    }
    server->listener = ncl_socket_listen(0, err, sizeof(err));
    if (server->listener == NULL || server->mutex == NULL ||
        server->cond == NULL) {
        ncl_fake_server_stop(server);
        return NULL;
    }
    server->port = ncl_socket_local_port(server->listener);
    server->thread = ncl_thread_start(fake_server_thread, server);
    if (server->thread == NULL) {
        ncl_fake_server_stop(server);
        return NULL;
    }
    return server;
}

unsigned ncl_fake_server_port(const ncl_fake_server *server)
{
    return server != NULL ? server->port : 0;
}

void ncl_fake_server_stop(ncl_fake_server *server)
{
    if (server == NULL) {
        return;
    }
    server->stop = true;
    /* Two phase teardown: wake the blocked accept() first, join the session
     * thread, and only then release the sockets it may still be reading. */
    if (server->listener != NULL) {
        ncl_socket_shutdown(server->listener);
    }
    if (server->client != NULL) {
        ncl_socket_shutdown(server->client);
    }
    if (server->thread != NULL) {
        ncl_thread_join(server->thread);
        server->thread = NULL;
    }
    if (server->listener != NULL) {
        ncl_socket *listener = server->listener;
        server->listener = NULL;
        ncl_socket_close(listener);
    }
    if (server->client != NULL) {
        ncl_socket *client = server->client;
        server->client = NULL;
        ncl_socket_close(client);
    }
    if (server->cond != NULL) {
        ncl_cond_destroy(server->cond);
    }
    if (server->mutex != NULL) {
        ncl_mutex_destroy(server->mutex);
    }
    free(server);
}

int ncl_fake_server_connection_count(const ncl_fake_server *server)
{
    return server != NULL ? server->connections : 0;
}

size_t ncl_fake_server_request_count(const ncl_fake_server *server)
{
    return server != NULL ? server->requests : 0;
}

const char *ncl_fake_server_last_topic(const ncl_fake_server *server)
{
    return server != NULL ? server->last_topic : "";
}

const char *ncl_fake_server_last_payload(const ncl_fake_server *server)
{
    return server != NULL ? server->last_payload : "";
}

size_t ncl_fake_server_last_publish(ncl_fake_server *server, char *topic,
                                    size_t topic_size, char *payload,
                                    size_t payload_size)
{
    size_t sequence;

    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(server->mutex);
    sequence = server->requests;
    if (topic != NULL && topic_size > 0) {
        snprintf(topic, topic_size, "%s", server->last_topic);
    }
    if (payload != NULL && payload_size > 0) {
        snprintf(payload, payload_size, "%s", server->last_payload);
    }
    ncl_mutex_unlock(server->mutex);
    return sequence;
}

size_t ncl_fake_server_publish_seq(ncl_fake_server *server)
{
    size_t sequence;
    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(server->mutex);
    sequence = server->requests;
    ncl_mutex_unlock(server->mutex);
    return sequence;
}

bool ncl_fake_server_publish_at(ncl_fake_server *server, size_t index,
                                char *topic, size_t topic_size, char *payload,
                                size_t payload_size)
{
    bool found = false;

    if (server == NULL || index >= server->history_count ||
        index >= NCL_FAKE_HISTORY) {
        return false;
    }
    ncl_mutex_lock(server->mutex);
    if (index < server->history_count && index < NCL_FAKE_HISTORY) {
        size_t slot = (server->history_count - 1 - index) % NCL_FAKE_HISTORY;
        if (topic != NULL && topic_size > 0) {
            snprintf(topic, topic_size, "%s", server->history[slot].topic);
        }
        if (payload != NULL && payload_size > 0) {
            snprintf(payload, payload_size, "%s", server->history[slot].payload);
        }
        found = true;
    }
    ncl_mutex_unlock(server->mutex);
    return found;
}

int ncl_fake_server_subscribe_count(const ncl_fake_server *server)
{
    return server != NULL ? server->subscribes : 0;
}

int ncl_fake_server_publish_count(const ncl_fake_server *server, int qos)
{
    if (server == NULL || qos < 0 || qos > 2) {
        return 0;
    }
    return server->publish_count[qos];
}

bool ncl_fake_server_wait(ncl_fake_server *server,
                          bool (*predicate)(ncl_fake_server *),
                          unsigned timeout_ms)
{
    int64_t deadline;
    bool ok;

    if (server == NULL || predicate == NULL) {
        return false;
    }
    deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    ncl_mutex_lock(server->mutex);
    while (!predicate(server)) {
        int64_t now = ncl_time_monotonic_millis();
        if (now >= deadline) {
            break;
        }
        ncl_cond_wait_timeout(server->cond, server->mutex,
                              (unsigned)(deadline - now));
    }
    ok = predicate(server);
    ncl_mutex_unlock(server->mutex);
    return ok;
}
