/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * TLS transport test: an in-process TLS server (OpenSSL) plus the MQTT client
 * over "ssl://".
 *
 * The suite only does something when the library was built with TLS support
 * (CMake: -DNCLINK_WITH_TLS=ON); otherwise it reports a skip and succeeds, so
 * the default zero-dependency build still runs the whole test set.
 *
 * Covered: TLS handshake, MQTT CONNECT/CONNACK over the encrypted channel,
 * SUBSCRIBE/SUBACK, PUBLISH at QoS 1 with a 8 KB payload (several TLS records),
 * an inbound PUBLISH pushed by the server, certificate verification against a
 * matching CA (success) and a foreign CA (must fail), and the documented
 * insecure mode (verify_peer = false).
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ncl_test.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"

#if defined(NCL_WITH_TLS)

#  include <openssl/err.h>
#  include <openssl/ssl.h>

#  if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
typedef SOCKET test_sock;
#    define TEST_INVALID_SOCK INVALID_SOCKET
#  else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <unistd.h>
typedef int test_sock;
#    define TEST_INVALID_SOCK (-1)
#  endif

#ifndef NCL_TEST_DATA_DIR
#  define NCL_TEST_DATA_DIR "../tests/data"
#endif

#define TLS_TIMEOUT_MS 8000

typedef struct {
    test_sock    listener;
    unsigned     port;
    ncl_thread  *thread;
    volatile bool stop;
    const char  *cert;
    const char  *key;

    /* observations */
    ncl_mutex *mutex;
    ncl_cond  *cond;
    int        sessions;
    int        connects;      /* MQTT CONNECT packets */
    int        subscribes;
    int        publishes;
    int        handshake_failures;
    char       last_topic[256];
    size_t     last_payload_len;
} tls_server;

static bool g_trace = false;

static void srv_close(test_sock s)
{
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

static bool srv_listen(tls_server *server, char *err, size_t err_len)
{
    struct sockaddr_in addr;
    int on = 1;

    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener == TEST_INVALID_SOCK) {
        snprintf(err, err_len, "socket() failed");
        return false;
    }
    setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
               (int)sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral */
    if (bind(server->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server->listener, 4) != 0) {
        snprintf(err, err_len, "bind/listen failed");
        srv_close(server->listener);
        server->listener = TEST_INVALID_SOCK;
        return false;
    }
    {
        struct sockaddr_in bound;
#if defined(_WIN32)
        int len = (int)sizeof(bound);
#else
        socklen_t len = (socklen_t)sizeof(bound);
#endif
        if (getsockname(server->listener, (struct sockaddr *)&bound, &len) != 0) {
            snprintf(err, err_len, "getsockname failed");
            srv_close(server->listener);
            server->listener = TEST_INVALID_SOCK;
            return false;
        }
        server->port = ntohs(bound.sin_port);
    }
    return true;
}

/** Serve one session: MQTT CONNECT/SUBSCRIBE/PUBLISH answers over TLS. */
static void srv_session(tls_server *server, SSL *ssl)
{
    unsigned char body[16 * 1024];
    bool pushed = false;

    for (;;) {
        unsigned char first = 0;
        int got = SSL_read(ssl, &first, 1);
        uint8_t type;
        uint32_t remaining;
        size_t used;
        size_t have = 0;

        if (server->stop || got <= 0) {
            return;
        }
        type = (uint8_t)((first >> 4) & 0x0F);
        /* Remaining length: up to four continuation bytes. */
        {
            unsigned char encoded[4];
            size_t i = 0;

            remaining = 0;
            for (;;) {
                if (SSL_read(ssl, &encoded[i], 1) != 1) {
                    return;
                }
                used = ncl_mqtt_varint_decode(encoded, i + 1, &remaining);
                i++;
                if (used != 0 || i >= 4) {
                    break;
                }
            }
        }
        if (remaining > sizeof(body)) {
            return; /* the test never sends that much */
        }
        while (have < remaining) {
            int n = SSL_read(ssl, body + have, (int)(remaining - have));
            if (n <= 0) {
                return;
            }
            have += (size_t)n;
        }

        if (g_trace) {
            printf("    [server] <-- %s (%u bytes)\n",
                   ncl_mqtt_packet_type_name(type), (unsigned)remaining);
        }

        switch (type) {
        case NCL_MQTT_PKT_CONNECT: {
            /* CONNACK: connect flags 0, reason 0, no properties. */
            unsigned char connack[5] = {0x20, 0x03, 0x00, 0x00, 0x00};

            ncl_mutex_lock(server->mutex);
            server->connects++;
            ncl_cond_broadcast(server->cond);
            ncl_mutex_unlock(server->mutex);
            SSL_write(ssl, connack, 5);
            break;
        }
        case NCL_MQTT_PKT_SUBSCRIBE: {
            /* MQTT 5 payload: packet id, property block, filter and its QoS. */
            uint16_t id = (uint16_t)((body[0] << 8) | body[1]);
            size_t p = 2;
            uint32_t props_len = 0;
            size_t used = ncl_mqtt_varint_decode(body + p, remaining - p,
                                                 &props_len);
            size_t filter_len;
            uint8_t qos;
            unsigned char suback[8];

            p += used != 0 ? used : 1;
            p += props_len;
            filter_len = (size_t)((body[p] << 8) | body[p + 1]);
            p += 2;
            qos = body[p + filter_len];
            if (filter_len < sizeof(server->last_topic)) {
                ncl_mutex_lock(server->mutex);
                memcpy(server->last_topic, body + p, filter_len);
                server->last_topic[filter_len] = '\0';
                server->subscribes++;
                ncl_cond_broadcast(server->cond);
                ncl_mutex_unlock(server->mutex);
            }
            /* MQTT 5: packet id, an empty property block, then the reason. */
            suback[0] = 0x90;
            suback[1] = 0x04;
            suback[2] = (unsigned char)(id >> 8);
            suback[3] = (unsigned char)(id & 0xFF);
            suback[4] = 0x00; /* property length */
            suback[5] = qos;  /* granted QoS */
            SSL_write(ssl, suback, 6);
            break;
        }
        case NCL_MQTT_PKT_PUBLISH: {
            size_t prefix = 2;
            size_t topic_len = (size_t)((body[0] << 8) | body[1]);
            uint16_t id = 0;
            uint8_t qos = (uint8_t)((first >> 1) & 0x03);
            unsigned char puback[4];
            size_t payload_offset;
            uint32_t props_len = 0;
            size_t used;

            if (topic_len < sizeof(server->last_topic)) {
                ncl_mutex_lock(server->mutex);
                memcpy(server->last_topic, body + prefix, topic_len);
                server->last_topic[topic_len] = '\0';
                server->publishes++;
                ncl_cond_broadcast(server->cond);
                ncl_mutex_unlock(server->mutex);
            }
            prefix += topic_len;
            if (qos > 0) {
                id = (uint16_t)((body[prefix] << 8) | body[prefix + 1]);
                prefix += 2;
            }
            /* MQTT 5 puts a property block between the header and the payload. */
            used = ncl_mqtt_varint_decode(body + prefix, remaining - prefix,
                                          &props_len);
            prefix += used != 0 ? used : 1;
            prefix += props_len;
            payload_offset = prefix;
            ncl_mutex_lock(server->mutex);
            server->last_payload_len = remaining - payload_offset;
            ncl_mutex_unlock(server->mutex);
            if (qos == 1) {
                puback[0] = 0x40;
                puback[1] = 0x02;
                puback[2] = (unsigned char)(id >> 8);
                puback[3] = (unsigned char)(id & 0xFF);
                SSL_write(ssl, puback, 4);
            }

            /* Push one message back so inbound delivery over TLS is covered. */
            if (!pushed) {
                const char *topic = "Tls/Response/V1";
                const char *payload = "{\"@id\":\"tls-1\"}";
                unsigned char packet[64];
                size_t n = 0;

                pushed = true;
                packet[n++] = 0x30; /* PUBLISH, QoS 0 */
                packet[n++] = (unsigned char)(2 + strlen(topic) + 1 +
                                              strlen(payload));
                packet[n++] = (unsigned char)(strlen(topic) >> 8);
                packet[n++] = (unsigned char)(strlen(topic) & 0xFF);
                memcpy(packet + n, topic, strlen(topic));
                n += strlen(topic);
                packet[n++] = 0x00; /* empty property block */
                memcpy(packet + n, payload, strlen(payload));
                n += strlen(payload);
                SSL_write(ssl, packet, (int)n);
            }
            break;
        }
        case NCL_MQTT_PKT_PINGREQ: {
            unsigned char pingresp[2] = {0xD0, 0x00};

            SSL_write(ssl, pingresp, 2);
            break;
        }
        default:
            break;
        }
    }
}

static void srv_thread(void *arg)
{
    tls_server *server = (tls_server *)arg;
    SSL_CTX *ctx;

    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        return;
    }
    if (SSL_CTX_use_certificate_file(ctx, server->cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, server->key, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return;
    }

    while (!server->stop) {
        struct timeval tv;
        fd_set set;
        int ready;
        test_sock client;
        SSL *ssl;

        FD_ZERO(&set);
        FD_SET(server->listener, &set);
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
#if defined(_WIN32)
        ready = select(0, &set, NULL, NULL, &tv);
#else
        ready = select((int)server->listener + 1, &set, NULL, NULL, &tv);
#endif
        if (ready <= 0) {
            continue;
        }
        client = accept(server->listener, NULL, NULL);
        if (client == TEST_INVALID_SOCK) {
            continue;
        }
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, (int)client);
        if (SSL_accept(ssl) == 1) {
            ncl_mutex_lock(server->mutex);
            server->sessions++;
            ncl_cond_broadcast(server->cond);
            ncl_mutex_unlock(server->mutex);
            srv_session(server, ssl);
            SSL_shutdown(ssl);
        } else {
            ncl_mutex_lock(server->mutex);
            server->handshake_failures++;
            ncl_cond_broadcast(server->cond);
            ncl_mutex_unlock(server->mutex);
        }
        SSL_free(ssl);
        srv_close(client);
    }
    SSL_CTX_free(ctx);
}

static bool srv_start(tls_server *server)
{
    char err[128];
    char path[512];

    memset(server, 0, sizeof(*server));
    server->mutex = ncl_mutex_create();
    server->cond = ncl_cond_create();
    server->listener = TEST_INVALID_SOCK;
    snprintf(path, sizeof(path), "%s/tls_localhost_cert.pem", NCL_TEST_DATA_DIR);
    server->cert = ncl_strdup(path);
    snprintf(path, sizeof(path), "%s/tls_localhost_key.pem", NCL_TEST_DATA_DIR);
    server->key = ncl_strdup(path);

    if (!srv_listen(server, err, sizeof(err))) {
        printf("    TLS server could not start: %s\n", err);
        return false;
    }
    server->thread = ncl_thread_start(srv_thread, server);
    return server->thread != NULL;
}

static void srv_stop(tls_server *server)
{
    server->stop = true;
    if (server->listener != TEST_INVALID_SOCK) {
        srv_close(server->listener);
        server->listener = TEST_INVALID_SOCK;
    }
    if (server->thread != NULL) {
        ncl_thread_join(server->thread);
        server->thread = NULL;
    }
    free((void *)server->cert);
    free((void *)server->key);
    ncl_cond_destroy(server->cond);
    ncl_mutex_destroy(server->mutex);
}

static bool srv_wait(tls_server *server, int *counter, int want, unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    bool ok;

    ncl_mutex_lock(server->mutex);
    while (*counter < want && ncl_time_monotonic_millis() < deadline) {
        int64_t left = deadline - ncl_time_monotonic_millis();
        ncl_cond_wait_timeout(server->cond, server->mutex,
                              (unsigned)(left > 0 ? left : 0));
    }
    ok = *counter >= want;
    ncl_mutex_unlock(server->mutex);
    return ok;
}

/* ------------------------------------------------------------- client side -- */

typedef struct {
    ncl_mutex *mutex;
    ncl_cond  *cond;
    int        messages;
    char       last_topic[256];
} tls_client_events;

static tls_client_events g_events;

/* NCL_TEST_TRACE=1 prints every MQTT packet the client exchanges. */
static void on_trace(void *user, bool inbound, ncl_mqtt_packet_type type,
                     size_t bytes)
{
    (void)user;
    if (g_trace) {
        printf("    [client] %s %-10s %u bytes\n", inbound ? "<--" : "-->",
               ncl_mqtt_packet_type_name(type), (unsigned)bytes);
    }
}

static void on_message(void *user, const ncl_mqtt_publish *publish)
{
    tls_client_events *events = (tls_client_events *)user;

    ncl_mutex_lock(events->mutex);
    events->messages++;
    snprintf(events->last_topic, sizeof(events->last_topic), "%s",
             publish->topic != NULL ? publish->topic : "");
    ncl_mutex_unlock(events->mutex);
    ncl_cond_broadcast(events->cond);
}

static char *data_path(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", NCL_TEST_DATA_DIR, name);
    return ncl_strdup(path);
}

static ncl_mqtt_client *make_client(const char *url, const char *ca,
                                    const char *server_name, bool verify)
{
    ncl_mqtt_client_options options;

    ncl_mqtt_client_options_default(&options);
    options.url = url;
    options.client_id = "ncl-tls-test";
    options.keep_alive_seconds = 5;
    options.connect_timeout_ms = 4000;
    options.automatic_reconnect = false;
    options.tls_ca_file = ca;
    options.tls_server_name = server_name;
    options.tls_verify_peer = verify;
    options.on_message = on_message;
    options.on_trace = on_trace;
    options.user = &g_events;
    return ncl_mqtt_client_create(&options);
}

#endif /* NCL_WITH_TLS */

NCL_TEST_MAIN_BEGIN()
{
#if defined(NCL_WITH_TLS)
    tls_server server;
    ncl_mqtt_client *client = NULL;
    char url[128];
    char *ca = NULL;
    char *other_ca = NULL;
    char *big = NULL;
    int granted = -1;
    size_t i;

    g_events.mutex = ncl_mutex_create();
    g_events.cond = ncl_cond_create();
    g_trace = getenv("NCL_TEST_TRACE") != NULL;
    NCL_CHECK(ncl_socket_tls_available());
    NCL_CHECK(srv_start(&server));
    if (server.listener == TEST_INVALID_SOCK) {
        return 1;
    }
    snprintf(url, sizeof(url), "ssl://127.0.0.1:%u", server.port);
    ca = data_path("tls_localhost_cert.pem");
    other_ca = data_path("tls_other_cert.pem");

    NCL_TEST_CASE("certificate verification rejects a foreign CA");
    client = make_client(url, other_ca, NULL, true);
    NCL_CHECK(client != NULL);
    NCL_CHECK(ncl_mqtt_client_connect(client) != NCL_OK);
    NCL_CHECK(!ncl_mqtt_client_is_connected(client));
    ncl_mqtt_client_destroy(client);
    NCL_CHECK(srv_wait(&server, &server.handshake_failures, 1, TLS_TIMEOUT_MS));

    NCL_TEST_CASE("host name verification rejects the wrong name");
    client = make_client(url, ca, "wrong.invalid", true);
    NCL_CHECK(client != NULL);
    NCL_CHECK(ncl_mqtt_client_connect(client) != NCL_OK);
    ncl_mqtt_client_destroy(client);

    NCL_TEST_CASE("handshake, CONNECT and SUBSCRIBE over TLS");
    client = make_client(url, ca, NULL, true);
    NCL_CHECK(client != NULL);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
    NCL_CHECK(ncl_mqtt_client_is_connected(client));
    NCL_CHECK(srv_wait(&server, &server.connects, 1, TLS_TIMEOUT_MS));
    NCL_CHECK_EQ_INT(ncl_mqtt_client_subscribe(client, "Tls/Response/#", 1,
                                               TLS_TIMEOUT_MS, &granted), NCL_OK);
    NCL_CHECK_EQ_INT(granted, 1);
    NCL_CHECK(srv_wait(&server, &server.subscribes, 1, TLS_TIMEOUT_MS));
    NCL_CHECK_EQ_STR(server.last_topic, "Tls/Response/#");

    NCL_TEST_CASE("an 8 KB payload crosses TLS record boundaries");
    big = (char *)malloc(8 * 1024);
    NCL_CHECK(big != NULL);
    if (big != NULL) {
        for (i = 0; i < 8 * 1024; i++) {
            big[i] = (char)('a' + (i % 26));
        }
        NCL_CHECK_EQ_INT(ncl_mqtt_client_publish(client, "Tls/Request", big,
                                                 8 * 1024, 1, NULL,
                                                 TLS_TIMEOUT_MS), NCL_OK);
        NCL_CHECK(srv_wait(&server, &server.publishes, 1, TLS_TIMEOUT_MS));
        NCL_CHECK_EQ_INT(server.last_payload_len, 8 * 1024);
    }

    NCL_TEST_CASE("a message pushed by the server arrives over TLS");
    {
        int64_t deadline = ncl_time_monotonic_millis() + TLS_TIMEOUT_MS;

        ncl_mutex_lock(g_events.mutex);
        while (g_events.messages < 1 &&
               ncl_time_monotonic_millis() < deadline) {
            int64_t left = deadline - ncl_time_monotonic_millis();
            ncl_cond_wait_timeout(g_events.cond, g_events.mutex,
                                  (unsigned)(left > 0 ? left : 0));
        }
        NCL_CHECK_EQ_INT(g_events.messages, 1);
        NCL_CHECK_EQ_STR(g_events.last_topic, "Tls/Response/V1");
        ncl_mutex_unlock(g_events.mutex);
    }

    NCL_CHECK_EQ_INT(ncl_mqtt_client_disconnect(client), NCL_OK);
    ncl_mqtt_client_destroy(client);

    NCL_TEST_CASE("verify_peer = false connects without a matching CA");
    client = make_client(url, other_ca, NULL, false);
    NCL_CHECK(client != NULL);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(client), NCL_OK);
    NCL_CHECK(ncl_mqtt_client_is_connected(client));
    NCL_CHECK_EQ_INT(ncl_mqtt_client_disconnect(client), NCL_OK);
    ncl_mqtt_client_destroy(client);

    free(big);
    free(ca);
    free(other_ca);
    srv_stop(&server);
    ncl_cond_destroy(g_events.cond);
    ncl_mutex_destroy(g_events.mutex);
#else
    printf("  skipped: this build has no TLS (configure with -DNCLINK_WITH_TLS=ON)\n");
#endif /* NCL_WITH_TLS */
}
NCL_TEST_MAIN_END()
