/*
 * Minimal NC-Link server used by the client tests.
 *
 * Speaks MQTT 5.0 on one socket (CONNECT/CONNACK, SUBSCRIBE/SUBACK, PUBLISH at
 * QoS 0/1/2, PINGREQ/PINGRESP, DISCONNECT) and forwards every inbound PUBLISH
 * to a user supplied handler so that a test can answer NC-Link requests.
 */
#ifndef NCL_FAKE_NCLINK_SERVER_H
#define NCL_FAKE_NCLINK_SERVER_H

#include "nclink/ncl_mqtt.h"

typedef struct ncl_fake_server ncl_fake_server;

typedef void (*ncl_fake_request_fn)(ncl_fake_server *server, void *user,
                                    const char *topic, const char *payload,
                                    size_t payload_len);

typedef struct {
    ncl_fake_request_fn on_request;
    void               *user;
} ncl_fake_server_options;

/** Start the server on an ephemeral port. Returns NULL on failure. */
ncl_fake_server *ncl_fake_server_start(const ncl_fake_server_options *options);

/** Port the server is listening on (0 when not started). */
unsigned ncl_fake_server_port(const ncl_fake_server *server);

/** Stop the server thread and release everything. */
void ncl_fake_server_stop(ncl_fake_server *server);

/** Publish a payload to the connected client. */
ncl_err ncl_fake_server_publish(ncl_fake_server *server, const char *topic,
                                const char *payload, int qos);

/** Number of accepted connections so far. */
int ncl_fake_server_connection_count(const ncl_fake_server *server);

size_t ncl_fake_server_request_count(const ncl_fake_server *server);

/** Most recent inbound topic and payload (NUL terminated copies). */
const char *ncl_fake_server_last_topic(const ncl_fake_server *server);
const char *ncl_fake_server_last_payload(const ncl_fake_server *server);

/**
 * Snapshot the most recent inbound PUBLISH atomically.
 * @return the sequence number of the captured message (0 when none has
 *         arrived), or the current sequence number when @p topic is NULL.
 *         Compare against a previously captured value to detect new traffic.
 */
size_t ncl_fake_server_last_publish(ncl_fake_server *server, char *topic,
                                    size_t topic_size, char *payload,
                                    size_t payload_size);

/** Total number of inbound PUBLISH packets observed. */
size_t ncl_fake_server_publish_seq(ncl_fake_server *server);

/**
 * Copy the @p index-th most recent inbound PUBLISH (0 = most recent) out of the
 * server's ring buffer. Returns false when @p index is beyond the history.
 * Useful for asserting on a burst of messages such as sample uploads.
 */
bool ncl_fake_server_publish_at(ncl_fake_server *server, size_t index,
                                char *topic, size_t topic_size, char *payload,
                                size_t payload_size);

/** Subscribe count observed from the client. */
int ncl_fake_server_subscribe_count(const ncl_fake_server *server);

/** Publish counts observed from the client, indexed by QoS. */
int ncl_fake_server_publish_count(const ncl_fake_server *server, int qos);

/** Wait until @p predicate is true, or @p timeout_ms elapses. */
bool ncl_fake_server_wait(ncl_fake_server *server,
                          bool (*predicate)(ncl_fake_server *),
                          unsigned timeout_ms);

#endif /* NCL_FAKE_NCLINK_SERVER_H */
