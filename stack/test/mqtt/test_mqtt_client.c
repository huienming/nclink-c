/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * End-to-end test of the MQTT 5.0 client against an in-process fake broker.
 *
 * The fake broker speaks the real wire protocol through the same packet codec
 * and asserts the byte level exchanges the client is supposed to produce:
 * CONNECT/CONNACK, SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PUBLISH at QoS 0/1/2
 * (including the PUBREL leg), inbound PUBLISH delivery, PINGREQ/PINGRESP and
 * DISCONNECT.
 */
#include "ncl_test.h"

#include <stdlib.h>

#include "nclink/ncl_mqtt.h"

/* ============================================================ fake broker == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    ncl_mutex  *mutex;
    ncl_cond   *cond;
    volatile bool stop;

    /* scripted behaviour */
    volatile bool drop_session;   /**< close the current session without DISCONNECT */
    volatile int  disconnect_reason; /**< >= 0: send DISCONNECT with it, then close */

    /* observations */
    int  connections;           /* accepted sessions */
    int  disconnect_sent;
    int  connect_count;
    int  subscribe_count;
    int  unsubscribe_count;
    int  pingreq_count;
    int  disconnect_count;
    int  publish_count[3];      /* by QoS */
    int  pubrel_count;
    int  puback_from_client;
    int  pubcomp_sent;
    int  connack_sent;

    int  last_sub_qos;
    char last_sub_topic[256];
    char last_unsub_topic[256];
    char last_topic[256];
    char last_payload[512];
    size_t last_payload_len;
    char last_user_property[256];
    int  last_publish_qos;

    /* injection */
    bool inject_pending;
    char inject_topic[256];
    char inject_payload[256];
} fake_broker;

static bool broker_read_packet(ncl_socket *sock, unsigned timeout_ms,
                               ncl_mqtt_packet_type *type, uint8_t *flags,
                               unsigned char **body, size_t *body_len)
{
    unsigned char first = 0;
    unsigned char length_bytes[4];
    size_t length_used = 0;
    uint32_t remaining = 0;
    unsigned char *buffer;
    int64_t deadline;
    int rc;

    rc = ncl_socket_recv(sock, &first, 1, timeout_ms);
    if (rc <= 0) {
        return false;
    }
    /* 首个字节已经拿到，剩下的**必须收全**：半途放弃会把后面那些字节当成新的报文头，
     * 整个会话从此错帧 —— 表现出来就是"某个计数一直不涨、等满超时"的偶发失败。
     * 所以这里用 2 s 的重试预算把剩余字节收完（真断了才认输）。 */
    deadline = ncl_time_monotonic_millis() + 2000;
    while (length_used < 4) {
        uint32_t value = 0;
        int64_t left = deadline - ncl_time_monotonic_millis();

        if (left <= 0) {
            return false;
        }
        rc = ncl_socket_recv(sock, &length_bytes[length_used], 1,
                             (unsigned)left);
        if (rc < 0) {
            return false;
        }
        if (rc == 0) {
            continue;   /* 这一字节还没到：接着等，别丢已经读到的东西 */
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
        ncl_socket_recv_exact(sock, buffer, remaining, 2000) != NCL_OK) {
        free(buffer);
        return false;
    }
    *type = (ncl_mqtt_packet_type)((first >> 4) & 0x0F);
    *flags = (uint8_t)(first & 0x0F);
    *body = buffer;
    *body_len = remaining;
    return true;
}

static void broker_send_connack(ncl_socket *sock, uint8_t reason)
{
    /* CONNACK: flags(0), reason, empty property block. */
    unsigned char packet[5] = {0x20, 0x03, 0x00, reason, 0x00};
    ncl_socket_send(sock, packet, sizeof(packet));
}

static void broker_handle_subscribe(fake_broker *broker, ncl_socket *sock,
                                    const unsigned char *body, size_t len)
{
    /* packet id, property length (0), topic filter, subscription options */
    uint16_t packet_id = (uint16_t)((body[0] << 8) | body[1]);
    size_t pos = 3; /* id (2) + property length byte */
    uint16_t topic_len = (uint16_t)((body[pos] << 8) | body[pos + 1]);
    unsigned char options = body[pos + 2 + topic_len];
    unsigned char suback[6];

    (void)len;
    ncl_mutex_lock(broker->mutex);
    broker->subscribe_count++;
    broker->last_sub_qos = options & 0x03;
    memcpy(broker->last_sub_topic, body + pos + 2, topic_len);
    broker->last_sub_topic[topic_len] = '\0';
    ncl_mutex_unlock(broker->mutex);

    /* SUBACK: id, empty properties, granted QoS. */
    suback[0] = 0x90;
    suback[1] = 0x04;
    suback[2] = (unsigned char)(packet_id >> 8);
    suback[3] = (unsigned char)(packet_id & 0xFF);
    suback[4] = 0x00;
    suback[5] = (unsigned char)(options & 0x03);
    ncl_socket_send(sock, suback, sizeof(suback));
}

static void broker_handle_unsubscribe(fake_broker *broker, ncl_socket *sock,
                                      const unsigned char *body, size_t len)
{
    uint16_t packet_id = (uint16_t)((body[0] << 8) | body[1]);
    size_t pos = 3;
    uint16_t topic_len = (uint16_t)((body[pos] << 8) | body[pos + 1]);
    unsigned char unsuback[6];

    (void)len;
    ncl_mutex_lock(broker->mutex);
    broker->unsubscribe_count++;
    memcpy(broker->last_unsub_topic, body + pos + 2, topic_len);
    broker->last_unsub_topic[topic_len] = '\0';
    ncl_mutex_unlock(broker->mutex);

    unsuback[0] = 0xB0;
    unsuback[1] = 0x04;
    unsuback[2] = (unsigned char)(packet_id >> 8);
    unsuback[3] = (unsigned char)(packet_id & 0xFF);
    unsuback[4] = 0x00; /* empty properties */
    unsuback[5] = 0x00; /* success */
    ncl_socket_send(sock, unsuback, sizeof(unsuback));
}

static void broker_handle_publish(fake_broker *broker, ncl_socket *sock,
                                 uint8_t flags, const unsigned char *body,
                                 size_t len)
{
    ncl_mqtt_publish publish;
    ncl_buffer ack;

    if (ncl_mqtt_decode_publish(flags, body, len, &publish) != NCL_OK) {
        return;
    }
    ncl_mutex_lock(broker->mutex);
    if (publish.qos >= 0 && publish.qos <= 2) {
        broker->publish_count[publish.qos]++;
    }
    broker->last_publish_qos = publish.qos;
    snprintf(broker->last_topic, sizeof(broker->last_topic), "%s", publish.topic);
    broker->last_payload_len = publish.payload_len < sizeof(broker->last_payload) - 1
                                   ? publish.payload_len
                                   : sizeof(broker->last_payload) - 1;
    memcpy(broker->last_payload, publish.payload, broker->last_payload_len);
    broker->last_payload[broker->last_payload_len] = '\0';
    {
        const char *version =
            ncl_mqtt_properties_get_user(&publish.properties, "version");
        snprintf(broker->last_user_property, sizeof(broker->last_user_property),
                 "%s", version != NULL ? version : "");
    }
    ncl_mutex_unlock(broker->mutex);

    if (publish.qos == 1) {
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBACK, publish.packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            ncl_socket_send(sock, ack.data, ack.len);
            ncl_buffer_free(&ack);
        }
    } else if (publish.qos == 2) {
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBREC, publish.packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            ncl_socket_send(sock, ack.data, ack.len);
            ncl_buffer_free(&ack);
        }
    }
    ncl_mqtt_publish_free(&publish);
}

static void broker_inject_publish(fake_broker *broker, ncl_socket *sock)
{
    char topic[256];
    char payload[256];

    ncl_mutex_lock(broker->mutex);
    if (!broker->inject_pending) {
        ncl_mutex_unlock(broker->mutex);
        return;
    }
    snprintf(topic, sizeof(topic), "%s", broker->inject_topic);
    snprintf(payload, sizeof(payload), "%s", broker->inject_payload);
    broker->inject_pending = false;
    ncl_mutex_unlock(broker->mutex);

    {
        ncl_buffer packet;
        ncl_mqtt_properties props;
        ncl_mqtt_properties_init(&props);
        ncl_mqtt_properties_add_user(&props, "version", "2.0");
        if (ncl_mqtt_encode_publish(topic, (const unsigned char *)payload,
                                    strlen(payload), 1, false, false, 1, &props,
                                    &packet) == NCL_OK) {
            ncl_socket_send(sock, packet.data, packet.len);
            ncl_buffer_free(&packet);
        }
        ncl_mqtt_properties_free(&props);
    }
}

static void broker_service(fake_broker *broker, ncl_socket *sock,
                           ncl_mqtt_packet_type type, uint8_t flags,
                           unsigned char *body, size_t body_len)
{
    switch (type) {
    case NCL_MQTT_PKT_CONNECT:
        ncl_mutex_lock(broker->mutex);
        broker->connect_count++;
        ncl_mutex_unlock(broker->mutex);
        broker_send_connack(sock, NCL_MQTT_REASON_SUCCESS);
        ncl_mutex_lock(broker->mutex);
        broker->connack_sent++;
        ncl_cond_broadcast(broker->cond);
        ncl_mutex_unlock(broker->mutex);
        break;

    case NCL_MQTT_PKT_SUBSCRIBE:
        broker_handle_subscribe(broker, sock, body, body_len);
        ncl_cond_broadcast(broker->cond);
        break;

    case NCL_MQTT_PKT_UNSUBSCRIBE:
        broker_handle_unsubscribe(broker, sock, body, body_len);
        ncl_cond_broadcast(broker->cond);
        break;

    case NCL_MQTT_PKT_PUBLISH:
        broker_handle_publish(broker, sock, flags, body, body_len);
        ncl_cond_broadcast(broker->cond);
        break;

    case NCL_MQTT_PKT_PUBREL: {
        uint16_t packet_id = 0;
        ncl_buffer ack;
        ncl_mqtt_decode_ack(body, body_len, &packet_id, NULL);
        ncl_mutex_lock(broker->mutex);
        broker->pubrel_count++;
        ncl_mutex_unlock(broker->mutex);
        if (ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBCOMP, packet_id,
                                NCL_MQTT_REASON_SUCCESS, &ack) == NCL_OK) {
            ncl_socket_send(sock, ack.data, ack.len);
            ncl_buffer_free(&ack);
            ncl_mutex_lock(broker->mutex);
            broker->pubcomp_sent++;
            ncl_cond_broadcast(broker->cond);
            ncl_mutex_unlock(broker->mutex);
        }
        break;
    }

    case NCL_MQTT_PKT_PUBACK:
        ncl_mutex_lock(broker->mutex);
        broker->puback_from_client++;
        ncl_cond_broadcast(broker->cond);
        ncl_mutex_unlock(broker->mutex);
        break;

    case NCL_MQTT_PKT_PINGREQ: {
        ncl_buffer resp;
        ncl_mutex_lock(broker->mutex);
        broker->pingreq_count++;
        ncl_cond_broadcast(broker->cond);
        ncl_mutex_unlock(broker->mutex);
        if (ncl_mqtt_encode_ping(true, &resp) == NCL_OK) {
            ncl_socket_send(sock, resp.data, resp.len);
            ncl_buffer_free(&resp);
        }
        break;
    }

    case NCL_MQTT_PKT_DISCONNECT:
        ncl_mutex_lock(broker->mutex);
        broker->disconnect_count++;
        ncl_cond_broadcast(broker->cond);
        ncl_mutex_unlock(broker->mutex);
        break;

    default:
        break;
    }
}

/* Serve one connection until the client goes away or the test asks for a drop. */
static void broker_session(fake_broker *broker, ncl_socket *sock)
{
    while (!broker->stop && !broker->drop_session &&
           broker->disconnect_reason < 0) {
        ncl_mqtt_packet_type type;
        uint8_t flags = 0;
        unsigned char *body = NULL;
        size_t body_len = 0;

        if (broker_read_packet(sock, 200, &type, &flags, &body, &body_len)) {
            broker_service(broker, sock, type, flags, body, body_len);
    free(body);
            if (type == NCL_MQTT_PKT_DISCONNECT) {
                break;
            }
            continue;
        }
        /* Idle tick: push an injected PUBLISH when asked to. */
        broker_inject_publish(broker, sock);
    }

    /* A server side DISCONNECT (0x8E and friends) is sent before closing. */
    if (broker->disconnect_reason >= 0 && !broker->stop) {
        ncl_buffer packet;
        ncl_mqtt_properties props;

        ncl_mqtt_properties_init(&props);
        if (ncl_mqtt_encode_disconnect((uint8_t)broker->disconnect_reason, &props,
                                       &packet) == NCL_OK) {
            ncl_socket_send(sock, packet.data, packet.len);
            ncl_buffer_free(&packet);
            ncl_mutex_lock(broker->mutex);
            broker->disconnect_sent++;
            ncl_cond_broadcast(broker->cond);
            ncl_mutex_unlock(broker->mutex);
        }
        ncl_mqtt_properties_free(&props);
    }
}

static void broker_thread(void *arg)
{
    fake_broker *broker = (fake_broker *)arg;

    /* Accepting repeatedly lets a test drop a session and watch the client
     * reconnect, which is what the reconnect cases need. */
    while (!broker->stop) {
        ncl_socket *listener = broker->listener;
        ncl_socket *sock;

        if (listener == NULL) {
            break;
        }
        sock = ncl_socket_accept(listener, 200);
        if (sock == NULL) {
            continue;
        }
        ncl_mutex_lock(broker->mutex);
        broker->connections++;
        broker->drop_session = false;
        broker->disconnect_reason = -1;
        ncl_cond_broadcast(broker->cond);
        ncl_mutex_unlock(broker->mutex);

        broker_session(broker, sock);
        ncl_socket_close(sock);
    }
}

static void broker_start(fake_broker *broker)
{
    char err[128];
    memset(broker, 0, sizeof(*broker));
    broker->mutex = ncl_mutex_create();
    broker->cond = ncl_cond_create();
    broker->disconnect_reason = -1;
    broker->listener = ncl_socket_listen(0, err, sizeof(err));
    NCL_CHECK(broker->listener != NULL);
    if (broker->listener == NULL) {
        return;
    }
    broker->port = ncl_socket_local_port(broker->listener);
    broker->thread = ncl_thread_start(broker_thread, broker);
}

static void broker_stop(fake_broker *broker)
{
    broker->stop = true;
    /* Two phase teardown, the same shape as fake_nclink_server.c: shut the
     * listener down to wake the blocked accept(), join the thread, and only
     * then release the socket the thread was still looking at. Calling
     * ncl_socket_close() here instead frees the object under accept() - that is
     * a use-after-free (caught by ASan at -O0). */
    if (broker->listener != NULL) {
        ncl_socket_shutdown(broker->listener);
    }
    if (broker->thread != NULL) {
        ncl_thread_join(broker->thread);
        broker->thread = NULL;
    }
    if (broker->listener != NULL) {
        ncl_socket *listener = broker->listener;
        broker->listener = NULL;
        ncl_socket_close(listener);
    }
    ncl_cond_destroy(broker->cond);
    ncl_mutex_destroy(broker->mutex);
}

/** Wait until @p predicate reports true, polling the broker condition. */
static bool broker_wait(fake_broker *broker, bool (*predicate)(fake_broker *),
                        unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    bool ok;

    ncl_mutex_lock(broker->mutex);
    while (!predicate(broker)) {
        int64_t now = ncl_time_monotonic_millis();
        if (now >= deadline) {
            break;
        }
        ncl_cond_wait_timeout(broker->cond, broker->mutex,
                              (unsigned)(deadline - now));
    }
    ok = predicate(broker);
    ncl_mutex_unlock(broker->mutex);
    return ok;
}

/* ---------------------------------------------------------- client side --- */

typedef struct {
    ncl_mutex *mutex;
    ncl_cond  *cond;
    int  connect_calls;
    int  disconnect_calls;
    bool last_reconnect_flag;
    uint8_t last_disconnect_reason;
    bool last_will_reconnect;
    int  message_count;
    char last_topic[256];
    char last_payload[512];
    int  last_qos;
} client_events;

static void on_connected(void *user, bool reconnect, const ncl_mqtt_connack *connack)
{
    client_events *events = (client_events *)user;
    ncl_mutex_lock(events->mutex);
    events->connect_calls++;
    events->last_reconnect_flag = reconnect;
    ncl_mutex_unlock(events->mutex);
    (void)connack;
}

static void on_disconnected(void *user, uint8_t reason_code, bool will_reconnect)
{
    client_events *events = (client_events *)user;
    ncl_mutex_lock(events->mutex);
    events->disconnect_calls++;
    events->last_disconnect_reason = reason_code;
    events->last_will_reconnect = will_reconnect;
    ncl_mutex_unlock(events->mutex);
}

static void on_message(void *user, const ncl_mqtt_publish *publish)
{
    client_events *events = (client_events *)user;
    ncl_mutex_lock(events->mutex);
    events->message_count++;
    snprintf(events->last_topic, sizeof(events->last_topic), "%s", publish->topic);
    events->last_qos = publish->qos;
    {
        size_t n = publish->payload_len < sizeof(events->last_payload) - 1
                       ? publish->payload_len
                       : sizeof(events->last_payload) - 1;
        memcpy(events->last_payload, publish->payload, n);
        events->last_payload[n] = '\0';
    }
    ncl_cond_broadcast(events->cond);
    ncl_mutex_unlock(events->mutex);
}

static bool client_wait_messages(client_events *events, int expected,
                                 unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    bool ok;

    ncl_mutex_lock(events->mutex);
    while (events->message_count < expected) {
        int64_t now = ncl_time_monotonic_millis();
        if (now >= deadline) {
            break;
        }
        ncl_cond_wait_timeout(events->cond, events->mutex,
                              (unsigned)(deadline - now));
    }
    ok = events->message_count >= expected;
    ncl_mutex_unlock(events->mutex);
    return ok;
}

static bool client_wait_disconnect(client_events *events, int expected,
                                   unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    bool ok;

    for (;;) {
        ncl_mutex_lock(events->mutex);
        ok = events->disconnect_calls >= expected;
        ncl_mutex_unlock(events->mutex);
        if (ok || ncl_time_monotonic_millis() >= deadline) {
            return ok;
        }
        ncl_sleep_millis(20);
    }
}

static bool pred_connected(fake_broker *broker)
{
    return broker->connack_sent > 0;
}

static bool pred_subscribed(fake_broker *broker)
{
    return broker->subscribe_count > 0;
}

static bool pred_unsubscribed(fake_broker *broker)
{
    return broker->unsubscribe_count > 0;
}

static bool pred_qos0(fake_broker *broker)
{
    return broker->publish_count[0] > 0;
}

static bool pred_qos1(fake_broker *broker)
{
    return broker->publish_count[1] > 0;
}

static bool pred_qos2_complete(fake_broker *broker)
{
    /* broker 收到 PUBREL 之后才发 PUBCOMP，而下面要断言 pubcomp_sent：
     * 只等 pubrel_count 会撞上"PUBREL 已记数、PUBCOMP 还没写完"的窗口（偶发失败），
     * 所以把 pubcomp_sent 也放进谓词。 */
    return broker->publish_count[2] > 0 && broker->pubrel_count > 0 &&
           broker->pubcomp_sent > 0;
}

static bool pred_ping(fake_broker *broker)
{
    return broker->pingreq_count > 0;
}

static bool pred_disconnect(fake_broker *broker)
{
    return broker->disconnect_count > 0;
}

static bool pred_inbound_puback(fake_broker *broker)
{
    return broker->puback_from_client > 0;
}

static bool pred_second_session(fake_broker *broker)
{
    return broker->connections >= 2;
}

static bool pred_server_disconnect(fake_broker *broker)
{
    return broker->disconnect_sent > 0;
}

/** Wait until the broker saw @p want more SUBSCRIBE packets than @p base. */
static bool wait_subscribes(fake_broker *broker, int base, int want,
                            unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;

    for (;;) {
        int count;

        ncl_mutex_lock(broker->mutex);
        count = broker->subscribe_count;
        ncl_mutex_unlock(broker->mutex);
        if (count >= base + want) {
            return true;
        }
        if (ncl_time_monotonic_millis() >= deadline) {
            return false;
        }
        ncl_sleep_millis(50);
    }
}

static int broker_subscribes(fake_broker *broker)
{
    int count;

    ncl_mutex_lock(broker->mutex);
    count = broker->subscribe_count;
    ncl_mutex_unlock(broker->mutex);
    return count;
}

static int broker_sessions(fake_broker *broker)
{
    int count;

    ncl_mutex_lock(broker->mutex);
    count = broker->connections;
    ncl_mutex_unlock(broker->mutex);
    return count;
}

/* ================================================================== tests == */

static void test_client_end_to_end(void)
{
    fake_broker broker;
    client_events events;
    ncl_mqtt_client_options options;
    ncl_mqtt_client *client;
    ncl_mqtt_properties props;
    char url[128];
    int granted = -1;

    memset(&events, 0, sizeof(events));
    events.mutex = ncl_mutex_create();
    events.cond = ncl_cond_create();
    broker_start(&broker);
    if (broker.listener == NULL && broker.port == 0) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", broker.port);

    NCL_TEST_CASE("client connects and triggers on_connect");
    ncl_mqtt_client_options_default(&options);
    options.url = url;
    options.client_id = "ncl-test-client";
    options.username = "admin";
    options.password = "123456";
    options.keep_alive_seconds = 2; /* exercise PINGREQ quickly */
    options.reconnect_delay_ms = 200;
    options.on_connect = on_connected;
    options.on_disconnect = on_disconnected;
    options.on_message = on_message;
    options.user = &events;

    client = ncl_mqtt_client_create(&options);
    NCL_CHECK(client != NULL);
    if (client == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
    NCL_CHECK(ncl_mqtt_client_is_connected(client));
    NCL_CHECK(broker_wait(&broker, pred_connected, 3000));
    NCL_CHECK_EQ_INT(events.connect_calls, 1);
    NCL_CHECK(events.last_reconnect_flag == false);

    NCL_TEST_CASE("subscribe receives the granted QoS");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, "Query/Response/V1", 2,
                                               3000, &granted),
                     NCL_OK);
    NCL_CHECK_EQ_INT(granted, 2);
    NCL_CHECK(broker_wait(&broker, pred_subscribed, 3000));
    NCL_CHECK_EQ_STR(broker.last_sub_topic, "Query/Response/V1");
    NCL_CHECK_EQ_INT(broker.last_sub_qos, 2);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscription_count(client), 1);

    NCL_TEST_CASE("QoS 0 publish is fire and forget");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, "Query/Request/V1", "{}", 2,
                                             0, NULL, 3000),
                     NCL_OK);
    NCL_CHECK(broker_wait(&broker, pred_qos0, 3000));
    NCL_CHECK_EQ_STR(broker.last_topic, "Query/Request/V1");

    NCL_TEST_CASE("QoS 1 publish waits for PUBACK and carries version=2.0");
    ncl_mqtt_properties_init(&props);
    ncl_mqtt_properties_add_user(&props, "version", "2.0");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, "Query/Request/V2",
                                             "{\"@id\":\"m1\"}", 12, 1, &props,
                                             3000),
                     NCL_OK);
    ncl_mqtt_properties_free(&props);
    NCL_CHECK(broker_wait(&broker, pred_qos1, 3000));
    NCL_CHECK_EQ_STR(broker.last_payload, "{\"@id\":\"m1\"}");
    NCL_CHECK_EQ_STR(broker.last_user_property, "2.0");
    NCL_CHECK_EQ_INT(broker.last_publish_qos, 1);

    NCL_TEST_CASE("QoS 2 publish completes the PUBLISH/PUBREL/PUBCOMP exchange");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, "Set/Request/V1", "{\"a\":1}",
                                             7, 2, NULL, 3000),
                     NCL_OK);
    NCL_CHECK(broker_wait(&broker, pred_qos2_complete, 3000));
    ncl_mutex_lock(broker.mutex);
    NCL_CHECK_EQ_INT(broker.publish_count[2], 1);
    NCL_CHECK_EQ_INT(broker.pubrel_count, 1);
    NCL_CHECK_EQ_INT(broker.pubcomp_sent, 1);
    ncl_mutex_unlock(broker.mutex);

    NCL_TEST_CASE("inbound PUBLISH reaches on_message and is acknowledged");
    ncl_mutex_lock(broker.mutex);
    snprintf(broker.inject_topic, sizeof(broker.inject_topic),
             "Query/Response/V1");
    snprintf(broker.inject_payload, sizeof(broker.inject_payload),
             "{\"@id\":\"m9\",\"values\":[]}");
    broker.inject_pending = true;
    ncl_mutex_unlock(broker.mutex);

    NCL_CHECK(client_wait_messages(&events, 1, 4000));
    NCL_CHECK_EQ_STR(events.last_topic, "Query/Response/V1");
    NCL_CHECK_EQ_STR(events.last_payload, "{\"@id\":\"m9\",\"values\":[]}");
    NCL_CHECK_EQ_INT(events.last_qos, 1);
    NCL_CHECK(broker_wait(&broker, pred_inbound_puback, 3000));

    NCL_TEST_CASE("keep alive emits PINGREQ and the broker replies PINGRESP");
    NCL_CHECK(broker_wait(&broker, pred_ping, 6000));
    NCL_CHECK(ncl_mqtt_client_is_connected(client));

    NCL_TEST_CASE("unsubscribe removes the topic filter");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_unsubscribe(client, "Query/Response/V1",
                                                3000),
                     NCL_OK);
    NCL_CHECK(broker_wait(&broker, pred_unsubscribed, 3000));
    NCL_CHECK_EQ_STR(broker.last_unsub_topic, "Query/Response/V1");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscription_count(client), 0);

    NCL_TEST_CASE("a dropped session reconnects and restores subscriptions");
    {
        int subs_before;
        int64_t started;

        NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, "Query/Response/V1", 1,
                                                   3000, &granted), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, "Set/Response/V1", 1,
                                                   3000, &granted), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, "Sample/V1/#", 2, 3000,
                                                   &granted), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_mqtt_client_subscription_count(client), 3);
        subs_before = broker_subscribes(&broker);

        /* Kill the session without a DISCONNECT: the client is expected to come
         * back on its own and re-send the three SUBSCRIBEs. */
        broker.drop_session = true;
        NCL_CHECK(broker_wait(&broker, pred_second_session, 6000));
        NCL_CHECK(wait_subscribes(&broker, subs_before, 3, 6000));

        /* Restoring must not stall the reader thread: before the fix it waited
         * one subscribe timeout per topic, so this publish timed out. */
        started = ncl_time_monotonic_millis();
        NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, "Query/Request/V3", "{}",
                                                 2, 1, NULL, 3000), NCL_OK);
        NCL_CHECK((ncl_time_monotonic_millis() - started) < 2000);
    }

    NCL_TEST_CASE("a server DISCONNECT with 0x8E stops the reconnect");
    {
        int sessions_before;
        int subs_before;

        sessions_before = broker_sessions(&broker);
        subs_before = broker_subscribes(&broker);
        ncl_mutex_lock(events.mutex);
        events.disconnect_calls = 0;
        ncl_mutex_unlock(events.mutex);

        /* Taking the session over must be reported as 0x8E and must not start a
         * reconnect loop (two connections would take each other over forever). */
        broker.disconnect_reason = NCL_MQTT_REASON_SESSION_TAKEN_OVER;
        NCL_CHECK(broker_wait(&broker, pred_server_disconnect, 3000));
        NCL_CHECK(client_wait_disconnect(&events, 1, 3000));
        NCL_CHECK_EQ_INT(events.last_disconnect_reason,
                         NCL_MQTT_REASON_SESSION_TAKEN_OVER);
        NCL_CHECK(events.last_will_reconnect == false);
        ncl_sleep_millis(1200); /* long enough for an unwanted reconnect */
        NCL_CHECK_EQ_INT(broker_sessions(&broker), sessions_before);
        NCL_CHECK(!ncl_mqtt_client_is_connected(client));

        /* An explicit connect takes the identity back and restores the topics. */
        NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
        NCL_CHECK(ncl_mqtt_client_is_connected(client));
        NCL_CHECK(wait_subscribes(&broker, subs_before, 3, 6000));
    }

    NCL_TEST_CASE("disconnect sends DISCONNECT and closes the session");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_disconnect(client), NCL_OK);
    NCL_CHECK(!ncl_mqtt_client_is_connected(client));
    NCL_CHECK(broker_wait(&broker, pred_disconnect, 3000));

    NCL_TEST_CASE("publishing while disconnected fails fast");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, "Query/Request/V1", "{}", 2,
                                             1, NULL, 500),
                     NCL_ERR_CLOSED);

    ncl_mqtt_client_destroy(client);
    broker_stop(&broker);
    ncl_cond_destroy(events.cond);
    ncl_mutex_destroy(events.mutex);
}

static void test_invalid_url(void)
{
    ncl_mqtt_client_options options;

    NCL_TEST_CASE("TLS and WebSocket URLs are rejected, bad ports too");
    ncl_mqtt_client_options_default(&options);
    options.client_id = "c";
    options.url = "ssl://broker:8883";
    if (ncl_socket_tls_available()) {
        /* A TLS capable build accepts the scheme; only the connect can fail. */
        ncl_mqtt_client *tls_client = ncl_mqtt_client_create(&options);
        options.connect_timeout_ms = 500;
        NCL_CHECK(tls_client != NULL);
        if (tls_client != NULL) {
            NCL_CHECK(ncl_mqtt_client_connect(tls_client) != NCL_OK);
            ncl_mqtt_client_destroy(tls_client);
        }
    } else {
        NCL_CHECK(ncl_mqtt_client_create(&options) == NULL);
    }

    options.url = "ws://broker:80";
    NCL_CHECK(ncl_mqtt_client_create(&options) == NULL);

    options.url = "tcp://broker:0";
    NCL_CHECK(ncl_mqtt_client_create(&options) == NULL);

    NCL_TEST_CASE("host and port are parsed from the URL");
    {
        char *host = NULL;
        unsigned port = 0;
        bool tls = true;
        NCL_CHECK_EQ_INT(ncl_socket_parse_url("tcp://iot.hz2025.com:1883", &host,
                                              &port, &tls),
                         NCL_OK);
        NCL_CHECK_EQ_STR(host, "iot.hz2025.com");
        NCL_CHECK_EQ_INT(port, 1883);
        NCL_CHECK(tls == false);
        ncl_free_safe(host);

        NCL_CHECK_EQ_INT(ncl_socket_parse_url("mqtt://localhost", &host, &port,
                                              &tls),
                         NCL_OK);
        NCL_CHECK_EQ_STR(host, "localhost");
        NCL_CHECK_EQ_INT(port, 1883);
        ncl_free_safe(host);

        NCL_CHECK_EQ_INT(ncl_socket_parse_url("ssl://secure:8883", &host, &port,
                                              &tls),
                         NCL_OK);
        NCL_CHECK(tls == true);
        NCL_CHECK_EQ_INT(port, 8883);
        ncl_free_safe(host);
    }
}

static void test_connect_failure(void)
{
    ncl_mqtt_client_options options;
    ncl_mqtt_client *client;

    NCL_TEST_CASE("connecting to a closed port reports failure");
    ncl_mqtt_client_options_default(&options);
    options.client_id = "c";
    /* Port 1 is reserved and not listening. */
    options.url = "tcp://127.0.0.1:1";
    options.connect_timeout_ms = 1500;
    options.automatic_reconnect = false;
    client = ncl_mqtt_client_create(&options);
    NCL_CHECK(client != NULL);
    if (client != NULL) {
        NCL_CHECK(ncl_mqtt_client_connect(client) != NCL_OK);
        NCL_CHECK(!ncl_mqtt_client_is_connected(client));
        NCL_CHECK(strlen(ncl_mqtt_client_last_error(client)) > 0);
        ncl_mqtt_client_destroy(client);
    }
}

NCL_TEST_MAIN_BEGIN()
    test_client_end_to_end();
    test_invalid_url();
    test_connect_failure();
NCL_TEST_MAIN_END()
