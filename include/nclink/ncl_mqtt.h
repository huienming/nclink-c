/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - MQTT 5.0 wire protocol (packet codec).
 *
 * MQTT 5.0 is implemented directly so that the library keeps its zero
 * dependency property. The layer below the client is split in two:
 *
 *   ncl_mqtt_packet.h  - packet encode/decode (this file), no I/O
 *   ncl_mqtt_client    - sockets, keep alive, sessions, reconnect (next stage)
 */
#ifndef NCL_MQTT_H
#define NCL_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_codec.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- constants -- */

/** MQTT 5.0 protocol level (the "Protocol Level" field of CONNECT). */
#define NCL_MQTT_PROTOCOL_LEVEL 5
#define NCL_MQTT_PROTOCOL_NAME "MQTT"

/** Maximum value representable by the variable byte integer encoding. */
#define NCL_MQTT_MAX_REMAINING_LENGTH 268435455u

/** Control packet types. */
typedef enum {
    NCL_MQTT_PKT_CONNECT = 1,
    NCL_MQTT_PKT_CONNACK = 2,
    NCL_MQTT_PKT_PUBLISH = 3,
    NCL_MQTT_PKT_PUBACK = 4,
    NCL_MQTT_PKT_PUBREC = 5,
    NCL_MQTT_PKT_PUBREL = 6,
    NCL_MQTT_PKT_PUBCOMP = 7,
    NCL_MQTT_PKT_SUBSCRIBE = 8,
    NCL_MQTT_PKT_SUBACK = 9,
    NCL_MQTT_PKT_UNSUBSCRIBE = 10,
    NCL_MQTT_PKT_UNSUBACK = 11,
    NCL_MQTT_PKT_PINGREQ = 12,
    NCL_MQTT_PKT_PINGRESP = 13,
    NCL_MQTT_PKT_DISCONNECT = 14,
    NCL_MQTT_PKT_AUTH = 15
} ncl_mqtt_packet_type;

/** CONNACK / DISCONNECT reason codes (the subset the client acts upon). */
#define NCL_MQTT_REASON_SUCCESS 0x00
#define NCL_MQTT_REASON_UNSPECIFIED_ERROR 0x80
#define NCL_MQTT_REASON_MALFORMED_PACKET 0x81
#define NCL_MQTT_REASON_PROTOCOL_ERROR 0x82
#define NCL_MQTT_REASON_NOT_AUTHORIZED 0x87
#define NCL_MQTT_REASON_SERVER_UNAVAILABLE 0x88
#define NCL_MQTT_REASON_BAD_USER_NAME_OR_PASSWORD 0x86
#define NCL_MQTT_REASON_CLIENT_IDENTIFIER_NOT_VALID 0x85
#define NCL_MQTT_REASON_KEEP_ALIVE_TIMEOUT 0x8D
#define NCL_MQTT_REASON_SESSION_TAKEN_OVER 0x8E
#define NCL_MQTT_REASON_TOPIC_ALIAS_INVALID 0x94
#define NCL_MQTT_REASON_PACKET_TOO_LARGE 0x95

/** Property identifiers. */
#define NCL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR 0x01
#define NCL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL 0x02
#define NCL_MQTT_PROP_CONTENT_TYPE 0x03
#define NCL_MQTT_PROP_RESPONSE_TOPIC 0x08
#define NCL_MQTT_PROP_CORRELATION_DATA 0x09
#define NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER 0x0B
#define NCL_MQTT_PROP_SESSION_EXPIRY_INTERVAL 0x11
#define NCL_MQTT_PROP_ASSIGNED_CLIENT_IDENTIFIER 0x12
#define NCL_MQTT_PROP_SERVER_KEEP_ALIVE 0x13
#define NCL_MQTT_PROP_AUTHENTICATION_METHOD 0x15
#define NCL_MQTT_PROP_AUTHENTICATION_DATA 0x16
#define NCL_MQTT_PROP_REQUEST_PROBLEM_INFORMATION 0x17
#define NCL_MQTT_PROP_WILL_DELAY_INTERVAL 0x18
#define NCL_MQTT_PROP_REQUEST_RESPONSE_INFORMATION 0x19
#define NCL_MQTT_PROP_RESPONSE_INFORMATION 0x1A
#define NCL_MQTT_PROP_SERVER_REFERENCE 0x1C
#define NCL_MQTT_PROP_REASON_STRING 0x1F
#define NCL_MQTT_PROP_RECEIVE_MAXIMUM 0x21
#define NCL_MQTT_PROP_TOPIC_ALIAS_MAXIMUM 0x22
#define NCL_MQTT_PROP_TOPIC_ALIAS 0x23
#define NCL_MQTT_PROP_MAXIMUM_QOS 0x24
#define NCL_MQTT_PROP_RETAIN_AVAILABLE 0x25
#define NCL_MQTT_PROP_USER_PROPERTY 0x26
#define NCL_MQTT_PROP_MAXIMUM_PACKET_SIZE 0x27
#define NCL_MQTT_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE 0x28
#define NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE 0x29
#define NCL_MQTT_PROP_SHARED_SUBSCRIPTION_AVAILABLE 0x2A

/* ------------------------------------------------------------ properties -- */

/**
 * MQTT 5.0 property set. Only the properties this library can originate are
 * modelled; unknown identifiers found on the wire are skipped safely while
 * parsing (the specification requires brokers to ignore them too).
 *
 * User properties keep insertion order, which matters because the NC-Link
 * protocol advertises itself with "version: 2.0".
 */
typedef struct {
    bool     has_payload_format_indicator;
    uint8_t  payload_format_indicator;

    bool     has_message_expiry_interval;
    uint32_t message_expiry_interval;

    char    *content_type;
    char    *response_topic;
    ncl_buffer correlation_data;

    bool     has_subscription_identifier;
    uint32_t subscription_identifier;

    bool     has_session_expiry_interval;
    uint32_t session_expiry_interval;
    char    *assigned_client_identifier;
    bool     has_server_keep_alive;
    uint16_t server_keep_alive;

    char    *authentication_method;
    ncl_buffer authentication_data;

    bool     has_request_problem_information;
    uint8_t  request_problem_information;
    bool     has_request_response_information;
    uint8_t  request_response_information;

    bool     has_will_delay_interval;
    uint32_t will_delay_interval;

    char    *response_information;
    char    *server_reference;
    char    *reason_string;

    bool     has_receive_maximum;
    uint16_t receive_maximum;
    bool     has_topic_alias_maximum;
    uint16_t topic_alias_maximum;
    bool     has_topic_alias;
    uint16_t topic_alias;

    bool     has_maximum_qos;
    uint8_t  maximum_qos;
    bool     has_retain_available;
    uint8_t  retain_available;

    /** User properties, stored as parallel key/value vectors. */
    ncl_strvec user_property_keys;
    ncl_strvec user_property_values;

    bool     has_maximum_packet_size;
    uint32_t maximum_packet_size;
    bool     has_wildcard_subscription_available;
    uint8_t  wildcard_subscription_available;
    bool     has_subscription_identifier_available;
    uint8_t  subscription_identifier_available;
    bool     has_shared_subscription_available;
    uint8_t  shared_subscription_available;
} ncl_mqtt_properties;

void ncl_mqtt_properties_init(ncl_mqtt_properties *props);
void ncl_mqtt_properties_free(ncl_mqtt_properties *props);

/** Append a user property, preserving order. */
ncl_err ncl_mqtt_properties_add_user(ncl_mqtt_properties *props,
                                     const char *key, const char *value);
/** First value of @p key, or NULL. */
const char *ncl_mqtt_properties_get_user(const ncl_mqtt_properties *props,
                                         const char *key);
bool ncl_mqtt_properties_has_user(const ncl_mqtt_properties *props,
                                  const char *key);

/**
 * Encode the property set as it appears inside a packet: a variable byte
 * length followed by the properties. An empty set encodes as the single byte
 * 0x00, so callers can test "has properties" with
 * `!(len == 1 && data[0] == 0)`.
 */
ncl_err ncl_mqtt_properties_write(const ncl_mqtt_properties *props, ncl_strbuf *out);

/** True when the encoded block produced by ncl_mqtt_properties_write holds no
 *  properties (i.e. is just the zero length byte). */
bool ncl_mqtt_properties_block_is_empty(const ncl_strbuf *block);

/**
 * Decode @p len bytes of properties.
 * Returns NCL_OK, or NCL_ERR_PARSE when the buffer is truncated.
 */
ncl_err ncl_mqtt_properties_read(const unsigned char *data, size_t len,
                                 ncl_mqtt_properties *props, size_t *consumed);

/* --------------------------------------------------------------- packets -- */

/** Options for CONNECT. */
typedef struct {
    const char *client_id;
    const char *username;   /**< optional */
    const char *password;   /**< optional */
    bool        clean_start;
    uint16_t    keep_alive_seconds;
    const ncl_mqtt_properties *properties; /**< optional */
    /* Will */
    const char *will_topic;   /**< optional */
    const unsigned char *will_payload;
    size_t      will_payload_len;
    int         will_qos;
    bool        will_retain;
    const ncl_mqtt_properties *will_properties; /**< optional */
} ncl_mqtt_connect_options;

/** Encode a complete CONNECT packet. */
ncl_err ncl_mqtt_encode_connect(const ncl_mqtt_connect_options *options,
                                ncl_buffer *out);

/** Decoded CONNACK. */
typedef struct {
    bool                session_present;
    uint8_t             reason_code;
    ncl_mqtt_properties properties;
} ncl_mqtt_connack;

void ncl_mqtt_connack_free(ncl_mqtt_connack *connack);
/** Decode a CONNACK body (the bytes after the fixed header). */
ncl_err ncl_mqtt_decode_connack(const unsigned char *body, size_t len,
                                ncl_mqtt_connack *out);

/** Encode a PUBLISH packet. */
ncl_err ncl_mqtt_encode_publish(const char *topic,
                                const unsigned char *payload, size_t payload_len,
                                int qos, bool retain, bool duplicate,
                                uint16_t packet_id,
                                const ncl_mqtt_properties *properties,
                                ncl_buffer *out);

/** Decoded PUBLISH. Topic is a NUL terminated copy; payload borrows @p body. */
typedef struct {
    bool        duplicate;
    int         qos;
    bool        retain;
    char       *topic;
    uint16_t    packet_id;
    const unsigned char *payload;
    size_t      payload_len;
    ncl_mqtt_properties properties;
} ncl_mqtt_publish;

void ncl_mqtt_publish_free(ncl_mqtt_publish *publish);
ncl_err ncl_mqtt_decode_publish(uint8_t header_flags, const unsigned char *body,
                                size_t len, ncl_mqtt_publish *out);

/** Encode an acknowledgement packet (PUBACK/PUBREC/PUBREL/PUBCOMP). */
ncl_err ncl_mqtt_encode_ack(ncl_mqtt_packet_type type, uint16_t packet_id,
                            uint8_t reason_code, ncl_buffer *out);

/** Decode an acknowledgement body: packet id plus optional reason/properties. */
ncl_err ncl_mqtt_decode_ack(const unsigned char *body, size_t len,
                            uint16_t *packet_id, uint8_t *reason_code);

/** Encode a SUBSCRIBE packet for a single topic filter. */
ncl_err ncl_mqtt_encode_subscribe(uint16_t packet_id, const char *topic_filter,
                                  int qos, const ncl_mqtt_properties *properties,
                                  ncl_buffer *out);

/** Encode an UNSUBSCRIBE packet for a single topic filter. */
ncl_err ncl_mqtt_encode_unsubscribe(uint16_t packet_id, const char *topic_filter,
                                    const ncl_mqtt_properties *properties,
                                    ncl_buffer *out);

/** Decoded SUBACK / UNSUBACK. reason_codes is heap allocated. */
typedef struct {
    uint16_t    packet_id;
    ncl_mqtt_properties properties;
    uint8_t    *reason_codes;
    size_t      reason_code_count;
} ncl_mqtt_suback;

void ncl_mqtt_suback_free(ncl_mqtt_suback *suback);
ncl_err ncl_mqtt_decode_suback(const unsigned char *body, size_t len,
                               ncl_mqtt_suback *out);

/** Encode PINGREQ (and, with @p response, PINGRESP). */
ncl_err ncl_mqtt_encode_ping(bool response, ncl_buffer *out);

/** Encode DISCONNECT with an optional reason code and properties. */
ncl_err ncl_mqtt_encode_disconnect(uint8_t reason_code,
                                   const ncl_mqtt_properties *properties,
                                   ncl_buffer *out);

/** Decoded DISCONNECT. */
typedef struct {
    uint8_t             reason_code;
    ncl_mqtt_properties properties;
} ncl_mqtt_disconnect;

void ncl_mqtt_disconnect_free(ncl_mqtt_disconnect *disconnect);
ncl_err ncl_mqtt_decode_disconnect(const unsigned char *body, size_t len,
                                   ncl_mqtt_disconnect *out);

/* ------------------------------------------------- fixed header utilities -- */

/** Encode a variable byte integer. Returns the number of bytes written (1-4). */
size_t ncl_mqtt_varint_encode(uint32_t value, unsigned char out[4]);

/**
 * Decode a variable byte integer.
 * @return bytes consumed (0 on malformed input).
 */
size_t ncl_mqtt_varint_decode(const unsigned char *data, size_t len,
                              uint32_t *value);

/** Peek at the fixed header of a buffer. Returns false when truncated. */
bool ncl_mqtt_peek_header(const unsigned char *data, size_t len,
                          ncl_mqtt_packet_type *type, uint8_t *flags,
                          uint32_t *remaining_length, size_t *header_len);

/** Human readable name of a packet type, used by the logger. */
const char *ncl_mqtt_packet_type_name(ncl_mqtt_packet_type type);

/* ====================================================== MQTT 5.0 client === */

/*
 * Transport layer: TCP connection, packet framing, keep alive, QoS 1/2 state
 * machines, subscription bookkeeping and automatic reconnect.
 *
 * Threading model: one background reader thread owns packet reception and the
 * protocol responses it triggers (PUBACK/PUBREC/PUBREL/PUBCOMP, PINGREQ,
 * reconnect). Caller threads may publish, subscribe and unsubscribe
 * concurrently; sends are serialised internally.
 */

typedef struct ncl_mqtt_client ncl_mqtt_client;

/** CONNACK received (also after a reconnect). */
typedef void (*ncl_mqtt_connected_fn)(void *user, bool reconnect,
                                      const ncl_mqtt_connack *connack);
/** Connection lost or closed; @p will_reconnect is true while a retry is
 *  scheduled. */
typedef void (*ncl_mqtt_disconnected_fn)(void *user, uint8_t reason_code,
                                         bool will_reconnect);
/** An inbound PUBLISH arrived; the payload is only valid for the call. */
typedef void (*ncl_mqtt_message_fn)(void *user, const ncl_mqtt_publish *publish);
/** Called for every packet, for tracing (may be NULL). */
typedef void (*ncl_mqtt_trace_fn)(void *user, bool inbound,
                                  ncl_mqtt_packet_type type, size_t bytes);

typedef struct {
    const char *url;        /**< "tcp://host:port" */
    const char *client_id;  /**< MQTT client identifier */
    const char *username;   /**< optional */
    const char *password;   /**< optional */
    bool        clean_start;
    unsigned    keep_alive_seconds;  /**< 0 disables keep alive */
    unsigned    connect_timeout_ms;  /**< default 10000 */
    bool        automatic_reconnect;
    unsigned    reconnect_delay_ms;  /**< base backoff, default 1000 */
    unsigned    reconnect_max_delay_ms; /**< default 30000 */
    ncl_mqtt_connected_fn    on_connect;
    ncl_mqtt_disconnected_fn on_disconnect;
    ncl_mqtt_message_fn      on_message;
    ncl_mqtt_trace_fn        on_trace;
    void       *user;
} ncl_mqtt_client_options;

/** Fill @p options with the defaults (clean start, 60 s keep alive, 10 s
 *  connect timeout, automatic reconnect). */
void ncl_mqtt_client_options_default(ncl_mqtt_client_options *options);

ncl_mqtt_client *ncl_mqtt_client_create(const ncl_mqtt_client_options *options);

/** Stop the reader thread and release every resource. */
void ncl_mqtt_client_destroy(ncl_mqtt_client *client);

/**
 * Open the TCP connection, send CONNECT and wait for CONNACK. Surviving
 * subscriptions are re-established when @p client was previously connected.
 * Returns NCL_OK only when the broker accepted the connection.
 */
ncl_err ncl_mqtt_client_connect(ncl_mqtt_client *client);

/** Send DISCONNECT and close the socket (no reconnect afterwards). */
ncl_err ncl_mqtt_client_disconnect(ncl_mqtt_client *client);

bool ncl_mqtt_client_is_connected(ncl_mqtt_client *client);

/**
 * Publish a message. For QoS 1 and 2 the call blocks until the broker
 * acknowledges (PUBACK / PUBCOMP) or @p timeout_ms elapses.
 */
ncl_err ncl_mqtt_client_publish(ncl_mqtt_client *client, const char *topic,
                                const void *payload, size_t payload_len, int qos,
                                const ncl_mqtt_properties *properties,
                                unsigned timeout_ms);

/** Subscribe and wait for the SUBACK. @p granted_qos receives the result when
 *  not NULL (0x80 indicates failure). */
ncl_err ncl_mqtt_client_subscribe(ncl_mqtt_client *client, const char *topic_filter,
                                  int qos, unsigned timeout_ms, int *granted_qos);

/** Unsubscribe and wait for the UNSUBACK. */
ncl_err ncl_mqtt_client_unsubscribe(ncl_mqtt_client *client,
                                    const char *topic_filter, unsigned timeout_ms);

/** Last transport level error message (never NULL). */
const char *ncl_mqtt_client_last_error(ncl_mqtt_client *client);

/** Number of topics the client is subscribed to (including while offline). */
size_t ncl_mqtt_client_subscription_count(ncl_mqtt_client *client);

/** Wait until the client is connected, up to @p timeout_ms (0 = forever). */
bool ncl_mqtt_client_wait_connected(ncl_mqtt_client *client, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MQTT_H */
