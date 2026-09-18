/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - client API.
 *
 * Three layers:
 *
 *   ncl_message_channel  - what the client needs from a transport: publish,
 *                          subscribe and unsubscribe. ncl_client_holder ships
 *                          the MQTT backed implementation.
 *   ncl_client           - per device serial number view of the protocol:
 *                          query/set/probe/method-call with request/response
 *                          correlation.
 *   ncl_client_holder    - process wide singleton that owns the MQTT
 *                          connection and the per-serial-number client map
 *                          (30 minute idle expiry).
 */
#ifndef NCL_CLIENT_H
#define NCL_CLIENT_H

#include <stdbool.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default response timeout in milliseconds. */
#define NCL_CLIENT_OPERATION_TIMEOUT 5000

/** Lifetime of a cached response, 5 minutes. */
#define NCL_CLIENT_RESPONSE_TTL_MS (5 * 60 * 1000)

/** Per device client idle lifetime, 30 minutes. */
#define NCL_CLIENT_IDLE_TTL_MS (30 * 60 * 1000)

/* ======================================================= message channel == */

typedef struct ncl_message_channel ncl_message_channel;

/**
 * Transport interface used by ncl_client. Implementations receive a pointer to
 * themselves so a struct can embed the vtable as its first member.
 *
 * publish() takes ownership of nothing: the channel serialises @p message.
 * Passing @p properties as NULL is allowed; the MQTT backed channel then adds
 * the NC-Link "version=2.0" user property itself.
 */
struct ncl_message_channel {
    ncl_err (*publish)(ncl_message_channel *self, const char *topic,
                       const ncl_message *message, int qos,
                       const ncl_mqtt_properties *properties);
    ncl_err (*subscribe)(ncl_message_channel *self, const char *topic, int qos);
    ncl_err (*unsubscribe)(ncl_message_channel *self, const char *topic);
};

static inline ncl_err ncl_channel_publish(ncl_message_channel *channel,
                                          const char *topic,
                                          const ncl_message *message, int qos,
                                          const ncl_mqtt_properties *properties)
{
    if (channel == NULL || channel->publish == NULL) {
        return NCL_ERR_STATE;
    }
    return channel->publish(channel, topic, message, qos, properties);
}

static inline ncl_err ncl_channel_subscribe(ncl_message_channel *channel,
                                            const char *topic, int qos)
{
    if (channel == NULL || channel->subscribe == NULL) {
        return NCL_ERR_STATE;
    }
    return channel->subscribe(channel, topic, qos);
}

static inline ncl_err ncl_channel_unsubscribe(ncl_message_channel *channel,
                                              const char *topic)
{
    if (channel == NULL || channel->unsubscribe == NULL) {
        return NCL_ERR_STATE;
    }
    return channel->unsubscribe(channel, topic);
}

/* ================================================================ client == */

typedef struct ncl_client ncl_client;

/** Create a client for @p sn bound to @p channel (not owned). */
ncl_client *ncl_client_create(const char *sn, ncl_message_channel *channel);
void        ncl_client_free(ncl_client *client);

const char *ncl_client_sn(const ncl_client *client);

/** Subscribe to every response topic for this serial number (QoS 2). */
ncl_err ncl_client_subscribe(ncl_client *client);
ncl_err ncl_client_unsubscribe(ncl_client *client);

/**
 * Deliver an inbound message. Takes ownership of @p message in every case.
 * Probe responses are cached by response topic, everything else by "@id".
 */
void ncl_client_on_message(ncl_client *client, const char *topic,
                           ncl_message *message);

/** Device model accessors (the model is borrowed, not owned). */
ncl_node *ncl_client_root_node(const ncl_client *client);
void      ncl_client_set_root_node(ncl_client *client, ncl_node *root_node);
/** Path -> id and id -> path lookups through the device model. */
char *ncl_client_get_id(ncl_client *client, const char *path);
char *ncl_client_get_path(ncl_client *client, const char *id);

/* Events ------------------------------------------------------------------ */

/**
 * Callback invoked for every message arriving on "Event/<sn>". @p event is
 * borrowed and released by the client as soon as the callback returns.
 */
typedef void (*ncl_client_event_fn)(ncl_client *client, const char *topic,
                                    const ncl_message *event, void *user);

/**
 * Subscribe to the device's event topic ("Event/<sn>"). Opt-in: it is not part
 * of ncl_client_subscribe().
 */
ncl_err ncl_client_subscribe_events(ncl_client *client, int qos);
ncl_err ncl_client_unsubscribe_events(ncl_client *client);

/** Install (or clear, with @p fn == NULL) the event callback. */
void ncl_client_set_event_handler(ncl_client *client, ncl_client_event_fn fn,
                                  void *user);

/** Number of event messages delivered to the callback so far. */
size_t ncl_client_event_count(const ncl_client *client);

/* Samples ----------------------------------------------------------------- */

/**
 * Callback invoked for every message arriving on "Sample/<sn>/<通道id>".
 * @p sample is borrowed and released by the client as soon as the callback
 * returns. It carries the channel id (`sample->as.sample.id`), the sampling and
 * upload intervals, a "paths" array, and one sample item per path holding the
 * values collected during the upload window.
 */
typedef void (*ncl_client_sample_fn)(ncl_client *client, const char *topic,
                                     const ncl_message *sample, void *user);

/**
 * Subscribe to every sample channel of this device ("Sample/<sn>/#"). Opt-in
 * like the event subscription.
 */
ncl_err ncl_client_subscribe_samples(ncl_client *client, int qos);
ncl_err ncl_client_unsubscribe_samples(ncl_client *client);

/** Install (or clear, with @p fn == NULL) the sample callback. */
void ncl_client_set_sample_handler(ncl_client *client, ncl_client_sample_fn fn,
                                   void *user);

/** Number of sample messages delivered to the callback so far. */
size_t ncl_client_sample_count(const ncl_client *client);

/** Topic filter used by ncl_client_subscribe_samples(), e.g. "Sample/<sn>/#". */
const char *ncl_client_sample_topic(const ncl_client *client);

/* Requests. Every function takes ownership of the request message and stores
 * the response in *out (caller frees with ncl_message_free). */

ncl_err ncl_client_ping(ncl_client *client, unsigned timeout_ms,
                        ncl_message **out);

ncl_err ncl_client_query(ncl_client *client, ncl_message *request,
                         unsigned timeout_ms, ncl_message **out);

ncl_err ncl_client_set(ncl_client *client, ncl_message *request,
                       unsigned timeout_ms, ncl_message **out);

/** Probe query: the response is matched by its response topic. */
ncl_err ncl_client_probe(ncl_client *client, unsigned timeout_ms,
                         ncl_message **out);

ncl_err ncl_client_probe_set(ncl_client *client, ncl_message *request,
                             unsigned timeout_ms, ncl_message **out);

ncl_err ncl_client_method_call(ncl_client *client, ncl_message *request,
                               unsigned timeout_ms, ncl_message **out);

/*
 * Asynchronous method call: the same request, but marked async. The device
 * answers with code=OK and a handler right away and runs the method on its own
 * worker; ask for progress with ncl_client_method_status() and for the outcome
 * with ncl_client_method_result(), both with that handler.
 */

/** Set "async" on @p request and issue it (takes ownership of the request). */
ncl_err ncl_client_method_call_async(ncl_client *client, ncl_message *request,
                                     unsigned timeout_ms, ncl_message **out);

/**
 * Method/Status query: @p object_id is the "id" field (the device id) and
 * @p handler the handle of the running call. The response carries process /
 * status / code.
 */
ncl_err ncl_client_method_status(ncl_client *client, const char *object_id,
                                 const char *handler, unsigned timeout_ms,
                                 ncl_message **out);

/**
 * Method/Result query: while the call runs the response is code=PENDING with no
 * "result"; once it finished the first query delivers code + "return" +
 * "result" (finished / error) and releases the handle.
 */
ncl_err ncl_client_method_result(ncl_client *client, const char *object_id,
                                 const char *handler, unsigned timeout_ms,
                                 ncl_message **out);

/* Convenience wrappers for the common single path operations. */

/** Read a single value: *out receives a clone of values[0]. */
ncl_err ncl_client_get_value(ncl_client *client, const char *path,
                             unsigned timeout_ms, ncl_json **out);

/** Read the values in the index range [start, end]. */
ncl_err ncl_client_get_value_range(ncl_client *client, const char *path,
                                   int start, int end, unsigned timeout_ms,
                                   ncl_json **out);

/** getLength(path, timeout). */
ncl_err ncl_client_get_length(ncl_client *client, const char *path,
                              unsigned timeout_ms, long long *out_length);

/**
 * setValue(path, value, timeout). Takes ownership of @p value.
 *
 * Returns NCL_OK only when every set response item reports code OK; anything
 * else means the device rejected the write or answered NG.
 */
ncl_err ncl_client_set_value(ncl_client *client, const char *path,
                             ncl_json *value, unsigned timeout_ms);

/** setValue(path, value, index, timeout). */
ncl_err ncl_client_set_value_index(ncl_client *client, const char *path,
                                   ncl_json *value, int index,
                                   unsigned timeout_ms);

/** addSample(config) / removeSample(id) method calls. */
ncl_err ncl_client_add_sample(ncl_client *client, const ncl_node *config,
                              unsigned timeout_ms);
ncl_err ncl_client_remove_sample(ncl_client *client, const char *id,
                                 unsigned timeout_ms);

/** True when the last operation completed within @p timeout_ms. */
bool ncl_client_is_ready(ncl_client *client);

/* ========================================================= client holder == */

/**
 * Options of the process wide MQTT connection.
 *
 * Everything except @c server_uri may be NULL/false, which keeps the default.
 * The @c tls_* fields are only used for "ssl://" / "tls://" URLs and only when
 * the library was built with NCLINK_WITH_TLS=ON (see ncl_socket_tls_available()).
 */
typedef struct {
    const char *server_uri;
    const char *username;
    const char *password;
    const char *tls_ca_file;      /**< PEM bundle; NULL = platform trust store */
    const char *tls_client_cert;  /**< optional PEM client certificate          */
    const char *tls_client_key;   /**< optional PEM key for the client cert     */
    const char *tls_server_name;  /**< SNI/host check; NULL = the URL host      */
    bool        tls_verify_peer;  /**< only read when tls_verify_peer_set       */
    bool        tls_verify_peer_set;
} ncl_client_holder_options;

/** Convenience initialiser: uri + credentials only, every other field default. */
void ncl_client_holder_options_default(ncl_client_holder_options *options);

/**
 * Initialise the process wide MQTT client with explicit options (TLS included).
 * Same connection policy as ncl_client_holder_init().
 */
ncl_err ncl_client_holder_init_ex(const ncl_client_holder_options *options);

/**
 * Initialise the process wide MQTT client.
 * The connection uses a random UUID client identifier, clean start, a 60 s keep
 * alive, a 10 s connect timeout and automatic reconnect.
 */
ncl_err ncl_client_holder_init(const char *server_uri, const char *username,
                               const char *password);

/** Fetch (creating on demand) the client for @p sn. Returns NULL before init. */
ncl_client *ncl_client_holder_get(const char *sn);

/** True once ncl_client_holder_init() succeeded. */
bool ncl_client_holder_is_initialised(void);

/** The underlying MQTT client, for diagnostics. */
ncl_mqtt_client *ncl_client_holder_mqtt(void);

/** Disconnect MQTT, drop every client and stop the FTP-less file hook. */
void ncl_client_holder_shutdown(void);

/** Number of live per-device clients (idle expiry runs on every access). */
size_t ncl_client_holder_client_count(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_CLIENT_H */
