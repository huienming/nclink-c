/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Interoperability test against a real MQTT 5.0 broker.
 *
 * Opt in by pointing NCL_TEST_MQTT_BROKER at one:
 *
 *     NCL_TEST_MQTT_BROKER=tcp://127.0.0.1:1883 ./test_broker
 *
 * Without that variable the suite reports a skip and succeeds, so the ordinary
 * build/test cycle needs no broker. tools/interop.sh starts EMQX and Mosquitto
 * in Docker and runs this suite against both.
 *
 * Covered: CONNECT/CONNACK, SUBSCRIBE at QoS 0/1/2 with the granted QoS,
 * PUBLISH at QoS 0/1/2, wildcard subscription, a payload past the single byte
 * remaining length limit (40 KB), UNSUBSCRIBE, keep alive pings while idle and
 * session takeover (a second client with the same identifier), after which the
 * first client must reconnect and re-subscribe on its own.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "ncl_test.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_platform.h"

#define BROKER_ENV "NCL_TEST_MQTT_BROKER"
#define INBOX_MAX (64 * 1024)
#define OP_TIMEOUT_MS 8000

typedef struct {
    ncl_mutex *mutex;
    ncl_cond  *cond;
    int        connect_count;
    int        reconnect_count;
    int        disconnect_count;
    uint8_t    last_disconnect_reason;
    int        message_count;
    char       last_topic[256];
    unsigned char payload[INBOX_MAX];
    size_t     payload_len;
} inbox;

static inbox g_inbox;
static bool  g_trace = false;
static int64_t g_start_ms = 0;

static void note(const char *fmt, ...)
{
    va_list ap;
    printf("    [%6lld ms] ", (long long)(ncl_time_monotonic_millis() -
                                          g_start_ms));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* NCL_TEST_TRACE=1 prints every packet: handy when a broker behaves oddly. */
static void on_trace(void *user, bool inbound, ncl_mqtt_packet_type type, size_t bytes)
{
    (void)user;
    if (g_trace) {
        note("[trace] %s %-12s %u bytes", inbound ? "<--" : "-->",
             ncl_mqtt_packet_type_name(type), (unsigned)bytes);
    }
}

static void inbox_reset(void)
{
    ncl_mutex_lock(g_inbox.mutex);
    g_inbox.message_count = 0;
    g_inbox.payload_len = 0;
    g_inbox.last_topic[0] = '\0';
    ncl_mutex_unlock(g_inbox.mutex);
}

static void on_connect(void *user, bool reconnect, const ncl_mqtt_connack *connack)
{
    inbox *box = (inbox *)user;

    ncl_mutex_lock(box->mutex);
    box->connect_count++;
    if (reconnect) {
        box->reconnect_count++;
    }
    ncl_mutex_unlock(box->mutex);
    if (connack->reason_code != 0) {
        printf("    broker refused CONNECT: reason=0x%02x\n", connack->reason_code);
    }
    ncl_cond_broadcast(box->cond);
}

static void on_disconnect(void *user, uint8_t reason_code, bool will_reconnect)
{
    inbox *box = (inbox *)user;

    ncl_mutex_lock(box->mutex);
    box->disconnect_count++;
    box->last_disconnect_reason = reason_code;
    ncl_mutex_unlock(box->mutex);
    printf("    disconnected (reason=0x%02x, will_reconnect=%d)\n",
           reason_code, will_reconnect ? 1 : 0);
    ncl_cond_broadcast(box->cond);
}

static void on_message(void *user, const ncl_mqtt_publish *publish)
{
    inbox *box = (inbox *)user;

    ncl_mutex_lock(box->mutex);
    box->message_count++;
    snprintf(box->last_topic, sizeof(box->last_topic), "%s",
             publish->topic != NULL ? publish->topic : "");
    box->payload_len = publish->payload_len < INBOX_MAX ? publish->payload_len
                                                        : INBOX_MAX;
    if (box->payload_len > 0 && publish->payload != NULL) {
        memcpy(box->payload, publish->payload, box->payload_len);
    }
    ncl_mutex_unlock(box->mutex);
    ncl_cond_broadcast(box->cond);
}

/* Wait until @p *counter reaches @p want, or the deadline passes. */
static bool wait_for(int *counter, int want, unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    bool ok;

    ncl_mutex_lock(g_inbox.mutex);
    while (*counter < want && ncl_time_monotonic_millis() < deadline) {
        int64_t left = deadline - ncl_time_monotonic_millis();
        ncl_cond_wait_timeout(g_inbox.cond, g_inbox.mutex,
                              (unsigned)(left > 0 ? left : 0));
    }
    ok = *counter >= want;
    ncl_mutex_unlock(g_inbox.mutex);
    return ok;
}

static bool wait_messages(int want, unsigned timeout_ms)
{
    return wait_for(&g_inbox.message_count, want, timeout_ms);
}

static bool wait_reconnect(int want, unsigned timeout_ms)
{
    return wait_for(&g_inbox.reconnect_count, want, timeout_ms);
}

static ncl_mqtt_client *make_client(const char *url, const char *client_id,
                                    unsigned keep_alive, bool auto_reconnect)
{
    ncl_mqtt_client_options options;

    ncl_mqtt_client_options_default(&options);
    options.url = url;
    options.client_id = client_id;
    options.clean_start = true;
    options.keep_alive_seconds = keep_alive;
    options.connect_timeout_ms = 5000;
    options.automatic_reconnect = auto_reconnect;
    options.reconnect_delay_ms = 200;
    options.reconnect_max_delay_ms = 1000;
    options.on_connect = on_connect;
    options.on_disconnect = on_disconnect;
    options.on_message = on_message;
    options.on_trace = on_trace;
    options.user = &g_inbox;
    return ncl_mqtt_client_create(&options);
}

NCL_TEST_MAIN_BEGIN()
{
    const char *url = getenv(BROKER_ENV);
    char base[160];
    char topic[224];
    char client_id[64];
    unsigned char *big = NULL;
    size_t big_len = 40 * 1024;
    ncl_mqtt_client *client = NULL;
    ncl_mqtt_client *rival = NULL;
    int granted = -1;
    size_t i;

    if (url == NULL || url[0] == '\0') {
        printf("  skipped: set %s=tcp://host:1883 to run this suite\n",
               BROKER_ENV);
        printf("%s: 0 checks, 0 failures\n", __FILE__);
        return 0;
    }

    g_inbox.mutex = ncl_mutex_create();
    g_inbox.cond = ncl_cond_create();
    g_start_ms = ncl_time_monotonic_millis();
    g_trace = getenv("NCL_TEST_TRACE") != NULL;

    /* tools/interop.sh polls with this mode: it only proves that the broker
     * accepts a CONNECT, so the suite starts once the broker is really up. */
    if (getenv("NCL_TEST_BROKER_PROBE") != NULL) {
        client = make_client(url, "ncl-interop-probe", 30, false);
        NCL_CHECK(client != NULL);
        NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
        ncl_mqtt_client_disconnect(client);
        ncl_mqtt_client_destroy(client);
        printf("  probe ok\n");
        printf("%s: %d checks, %d failures\n", __FILE__, ncl_test_checks,
               ncl_test_failures);
        ncl_cond_destroy(g_inbox.cond);
        ncl_mutex_destroy(g_inbox.mutex);
        return ncl_test_failures == 0 ? 0 : 1;
    }
    snprintf(client_id, sizeof(client_id), "ncl-interop-%u",
             (unsigned)(ncl_time_millis() % 100000000u));
    snprintf(base, sizeof(base), "ncl/interop/%s", client_id);

    NCL_TEST_CASE("connect to the broker");
    client = make_client(url, client_id, 2, true);
    NCL_CHECK(client != NULL);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
    NCL_CHECK(ncl_mqtt_client_is_connected(client));
    NCL_CHECK_EQ_INT(g_inbox.connect_count, 1);

    NCL_TEST_CASE("subscribe at QoS 0/1/2 and check the granted QoS");
    snprintf(topic, sizeof(topic), "%s/qos/q0", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, topic, 0, OP_TIMEOUT_MS,
                                               &granted), NCL_OK);
    NCL_CHECK_EQ_INT(granted, 0);
    snprintf(topic, sizeof(topic), "%s/qos/q1", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, topic, 1, OP_TIMEOUT_MS,
                                               &granted), NCL_OK);
    NCL_CHECK_EQ_INT(granted, 1);
    snprintf(topic, sizeof(topic), "%s/qos/q2", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, topic, 2, OP_TIMEOUT_MS,
                                               &granted), NCL_OK);
    NCL_CHECK_EQ_INT(granted, 2);

    NCL_TEST_CASE("publish and receive at QoS 0/1/2");
    inbox_reset();
    snprintf(topic, sizeof(topic), "%s/qos/q0", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "hello-q0", 8, 0,
                                             NULL, OP_TIMEOUT_MS), NCL_OK);
    snprintf(topic, sizeof(topic), "%s/qos/q1", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "hello-q1", 8, 1,
                                             NULL, OP_TIMEOUT_MS), NCL_OK);
    snprintf(topic, sizeof(topic), "%s/qos/q2", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "hello-q2", 8, 2,
                                             NULL, OP_TIMEOUT_MS), NCL_OK);
    NCL_CHECK(wait_messages(3, OP_TIMEOUT_MS));
    ncl_mutex_lock(g_inbox.mutex);
    printf("    received %d messages, last topic %s\n", g_inbox.message_count,
           g_inbox.last_topic);
    ncl_mutex_unlock(g_inbox.mutex);

    NCL_TEST_CASE("wildcard subscription");
    snprintf(topic, sizeof(topic), "%s/wild/#", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, topic, 1, OP_TIMEOUT_MS,
                                               &granted), NCL_OK);
    NCL_CHECK(ncl_mqtt_client_subscription_count(client) >= 4);
    inbox_reset();
    snprintf(topic, sizeof(topic), "%s/wild/a/b/c", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "deep", 4, 1, NULL,
                                             OP_TIMEOUT_MS), NCL_OK);
    NCL_CHECK(wait_messages(1, OP_TIMEOUT_MS));
    ncl_mutex_lock(g_inbox.mutex);
    NCL_CHECK_EQ_STR(g_inbox.last_topic, topic);
    ncl_mutex_unlock(g_inbox.mutex);

    NCL_TEST_CASE("payload past the single byte remaining length");
    big = (unsigned char *)malloc(big_len);
    NCL_CHECK(big != NULL);
    if (big != NULL) {
        for (i = 0; i < big_len; i++) {
            big[i] = (unsigned char)((i * 31u + 7u) & 0xFFu);
        }
        inbox_reset();
        snprintf(topic, sizeof(topic), "%s/qos/q1", base);
        NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, big, big_len, 1,
                                                 NULL, OP_TIMEOUT_MS * 2), NCL_OK);
        NCL_CHECK(wait_messages(1, OP_TIMEOUT_MS * 2));
        ncl_mutex_lock(g_inbox.mutex);
        NCL_CHECK_EQ_INT(g_inbox.payload_len, big_len);
        NCL_CHECK(g_inbox.payload_len == big_len &&
                  memcmp(g_inbox.payload, big, big_len) == 0);
        ncl_mutex_unlock(g_inbox.mutex);
    }

    NCL_TEST_CASE("unsubscribe stops the delivery");
    snprintf(topic, sizeof(topic), "%s/qos/q2", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_unsubscribe(client, topic, OP_TIMEOUT_MS),
                     NCL_OK);
    inbox_reset();
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "dropped", 7, 1, NULL,
                                             OP_TIMEOUT_MS), NCL_OK);
    ncl_sleep_millis(600);
    ncl_mutex_lock(g_inbox.mutex);
    NCL_CHECK_EQ_INT(g_inbox.message_count, 0);
    ncl_mutex_unlock(g_inbox.mutex);

    NCL_TEST_CASE("keep alive survives an idle period");
    ncl_sleep_millis(6000); /* keep alive is 2 s, so several PINGREQ went out */
    NCL_CHECK(ncl_mqtt_client_is_connected(client));
    NCL_CHECK_EQ_INT(g_inbox.disconnect_count, 0);
    snprintf(topic, sizeof(topic), "%s/qos/q2", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, topic, 2, OP_TIMEOUT_MS,
                                               &granted), NCL_OK);
    inbox_reset();
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "after-idle", 10, 1,
                                             NULL, OP_TIMEOUT_MS), NCL_OK);
    NCL_CHECK(wait_messages(1, OP_TIMEOUT_MS));

    NCL_TEST_CASE("session takeover is reported as 0x8E and does not flap");
    inbox_reset();
    rival = make_client(url, client_id, 40, false);
    NCL_CHECK(rival != NULL);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(rival), NCL_OK);
    /* The broker drops the first connection with "session taken over". Taking
     * it back automatically would start a takeover loop, so the client must
     * stay down until the application asks again. */
    NCL_CHECK(wait_for(&g_inbox.disconnect_count, 1, OP_TIMEOUT_MS));
    NCL_CHECK_EQ_INT(g_inbox.last_disconnect_reason,
                     NCL_MQTT_REASON_SESSION_TAKEN_OVER);
    ncl_sleep_millis(3000);
    NCL_CHECK_EQ_INT(g_inbox.reconnect_count, 0);
    NCL_CHECK(!ncl_mqtt_client_is_connected(client));

    NCL_TEST_CASE("an explicit connect takes the identity back");
    note("disconnecting the rival");
    ncl_mqtt_client_disconnect(rival); /* the broker already dropped it */
    note("destroying the rival");
    ncl_mqtt_client_destroy(rival);
    note("connecting again");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
    note("connected again");
    NCL_CHECK(ncl_mqtt_client_is_connected(client));
    inbox_reset();
    snprintf(topic, sizeof(topic), "%s/qos/q1", base);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, topic, "after-reconnect",
                                             16, 1, NULL, OP_TIMEOUT_MS), NCL_OK);
    NCL_CHECK(wait_messages(1, OP_TIMEOUT_MS));

    NCL_TEST_CASE("shut down cleanly");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_disconnect(client), NCL_OK);
    NCL_CHECK(!ncl_mqtt_client_is_connected(client));
    ncl_mqtt_client_destroy(client);

    free(big);
    ncl_cond_destroy(g_inbox.cond);
    ncl_mutex_destroy(g_inbox.mutex);
}
NCL_TEST_MAIN_END()
