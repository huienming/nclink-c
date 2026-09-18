/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - MQTT 5.0 packet codec. */
#include "nclink/ncl_mqtt.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"

/* ============================================================== utilities = */

size_t ncl_mqtt_varint_encode(uint32_t value, unsigned char out[4])
{
    size_t i = 0;
    do {
        unsigned char byte = (unsigned char)(value % 128);
        value /= 128;
        if (value > 0) {
            byte |= 0x80;
        }
        out[i++] = byte;
    } while (value > 0 && i < 4);
    return i;
}

size_t ncl_mqtt_varint_decode(const unsigned char *data, size_t len,
                              uint32_t *value)
{
    uint32_t multiplier = 1;
    uint32_t result = 0;
    size_t i;

    if (data == NULL || value == NULL) {
        return 0;
    }
    for (i = 0; i < len && i < 4; i++) {
        result += (uint32_t)(data[i] & 0x7F) * multiplier;
        if ((data[i] & 0x80) == 0) {
            *value = result;
            return i + 1;
        }
        multiplier *= 128;
    }
    return 0; /* truncated or > 4 bytes */
}

/* UTF-8 string with a two byte length prefix. */
static ncl_err ncl_mqtt_write_string(ncl_strbuf *out, const char *s)
{
    size_t len = s != NULL ? strlen(s) : 0;
    unsigned char prefix[2];

    if (len > 0xFFFFu) {
        return NCL_ERR_INVALID_ARG;
    }
    prefix[0] = (unsigned char)((len >> 8) & 0xFF);
    prefix[1] = (unsigned char)(len & 0xFF);
    if (ncl_strbuf_append(out, (const char *)prefix, 2) != NCL_OK) {
        return NCL_ERR_NOMEM;
    }
    if (len > 0 && ncl_strbuf_append(out, s, len) != NCL_OK) {
        return NCL_ERR_NOMEM;
    }
    return NCL_OK;
}

static ncl_err ncl_mqtt_write_binary(ncl_strbuf *out, const unsigned char *data,
                                     size_t len)
{
    unsigned char prefix[2];
    if (len > 0xFFFFu) {
        return NCL_ERR_INVALID_ARG;
    }
    prefix[0] = (unsigned char)((len >> 8) & 0xFF);
    prefix[1] = (unsigned char)(len & 0xFF);
    if (ncl_strbuf_append(out, (const char *)prefix, 2) != NCL_OK) {
        return NCL_ERR_NOMEM;
    }
    if (len > 0 && ncl_strbuf_append(out, (const char *)data, len) != NCL_OK) {
        return NCL_ERR_NOMEM;
    }
    return NCL_OK;
}

static ncl_err ncl_mqtt_write_u16(ncl_strbuf *out, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)((value >> 8) & 0xFF);
    bytes[1] = (unsigned char)(value & 0xFF);
    return ncl_strbuf_append(out, (const char *)bytes, 2) == NCL_OK
               ? NCL_OK
               : NCL_ERR_NOMEM;
}

static ncl_err ncl_mqtt_write_u32(ncl_strbuf *out, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)((value >> 24) & 0xFF);
    bytes[1] = (unsigned char)((value >> 16) & 0xFF);
    bytes[2] = (unsigned char)((value >> 8) & 0xFF);
    bytes[3] = (unsigned char)(value & 0xFF);
    return ncl_strbuf_append(out, (const char *)bytes, 4) == NCL_OK
               ? NCL_OK
               : NCL_ERR_NOMEM;
}

static ncl_err ncl_mqtt_write_property_id(ncl_strbuf *out, uint8_t id)
{
    /* Every id in this set fits in a single byte. */
    return ncl_strbuf_putc(out, (char)id) == NCL_OK ? NCL_OK : NCL_ERR_NOMEM;
}

/* Wrap a body with the fixed header (type/flags + remaining length). */
static ncl_err ncl_mqtt_frame(uint8_t type_and_flags, const ncl_strbuf *body,
                              ncl_buffer *out)
{
    unsigned char length_bytes[4];
    size_t length_len;
    size_t total;
    unsigned char *buffer;

    if (out == NULL || body == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (body->len > NCL_MQTT_MAX_REMAINING_LENGTH) {
        return NCL_ERR_RANGE;
    }
    length_len = ncl_mqtt_varint_encode((uint32_t)body->len, length_bytes);
    total = 1 + length_len + body->len;

    buffer = (unsigned char *)ncl_mem_alloc(total);
    if (buffer == NULL) {
        return NCL_ERR_NOMEM;
    }
    buffer[0] = type_and_flags;
    memcpy(buffer + 1, length_bytes, length_len);
    if (body->len > 0) {
        memcpy(buffer + 1 + length_len, body->data, body->len);
    }
    out->data = buffer;
    out->len = total;
    return NCL_OK;
}

/* ------------------------------------------------------------ read helpers */

typedef struct {
    const unsigned char *data;
    size_t               len;
    size_t               pos;
    bool                 truncated;
} ncl_mqtt_reader;

static void ncl_mqtt_reader_init(ncl_mqtt_reader *reader,
                                 const unsigned char *data, size_t len)
{
    reader->data = data;
    reader->len = len;
    reader->pos = 0;
    reader->truncated = false;
}

static uint8_t ncl_mqtt_read_u8(ncl_mqtt_reader *reader)
{
    if (reader->pos + 1 > reader->len) {
        reader->truncated = true;
        return 0;
    }
    return reader->data[reader->pos++];
}

static uint16_t ncl_mqtt_read_u16(ncl_mqtt_reader *reader)
{
    uint16_t value;
    if (reader->pos + 2 > reader->len) {
        reader->truncated = true;
        return 0;
    }
    value = (uint16_t)((reader->data[reader->pos] << 8) |
                       reader->data[reader->pos + 1]);
    reader->pos += 2;
    return value;
}

static uint32_t ncl_mqtt_read_u32(ncl_mqtt_reader *reader)
{
    uint32_t value;
    if (reader->pos + 4 > reader->len) {
        reader->truncated = true;
        return 0;
    }
    value = ((uint32_t)reader->data[reader->pos] << 24) |
            ((uint32_t)reader->data[reader->pos + 1] << 16) |
            ((uint32_t)reader->data[reader->pos + 2] << 8) |
            (uint32_t)reader->data[reader->pos + 3];
    reader->pos += 4;
    return value;
}

static uint32_t ncl_mqtt_read_varint(ncl_mqtt_reader *reader)
{
    uint32_t value = 0;
    size_t used;
    if (reader->pos >= reader->len) {
        reader->truncated = true;
        return 0;
    }
    used = ncl_mqtt_varint_decode(reader->data + reader->pos,
                                  reader->len - reader->pos, &value);
    if (used == 0) {
        reader->truncated = true;
        return 0;
    }
    reader->pos += used;
    return value;
}

/* Returns a heap copy of a length prefixed UTF-8 string. */
static char *ncl_mqtt_read_string(ncl_mqtt_reader *reader)
{
    uint16_t len = ncl_mqtt_read_u16(reader);
    char *copy;
    if (reader->truncated) {
        return NULL;
    }
    if (reader->pos + len > reader->len) {
        reader->truncated = true;
        return NULL;
    }
    copy = ncl_strndup((const char *)reader->data + reader->pos, len);
    reader->pos += len;
    return copy;
}

static ncl_err ncl_mqtt_read_binary(ncl_mqtt_reader *reader, ncl_buffer *out)
{
    uint16_t len = ncl_mqtt_read_u16(reader);
    if (reader->truncated) {
        return NCL_ERR_PARSE;
    }
    if (reader->pos + len > reader->len) {
        reader->truncated = true;
        return NCL_ERR_PARSE;
    }
    out->data = NULL;
    out->len = 0;
    if (len > 0) {
        out->data = (unsigned char *)ncl_mem_alloc(len);
        if (out->data == NULL) {
            return NCL_ERR_NOMEM;
        }
        memcpy(out->data, reader->data + reader->pos, len);
        out->len = len;
    }
    reader->pos += len;
    return NCL_OK;
}

/* ============================================================= properties = */

void ncl_mqtt_properties_init(ncl_mqtt_properties *props)
{
    if (props == NULL) {
        return;
    }
    memset(props, 0, sizeof(*props));
    ncl_strvec_init(&props->user_property_keys);
    ncl_strvec_init(&props->user_property_values);
}

void ncl_mqtt_properties_free(ncl_mqtt_properties *props)
{
    if (props == NULL) {
        return;
    }
    ncl_mem_free(props->content_type);
    ncl_mem_free(props->response_topic);
    ncl_buffer_free(&props->correlation_data);
    ncl_mem_free(props->assigned_client_identifier);
    ncl_mem_free(props->authentication_method);
    ncl_buffer_free(&props->authentication_data);
    ncl_mem_free(props->response_information);
    ncl_mem_free(props->server_reference);
    ncl_mem_free(props->reason_string);
    ncl_strvec_free(&props->user_property_keys);
    ncl_strvec_free(&props->user_property_values);
    memset(props, 0, sizeof(*props));
}

ncl_err ncl_mqtt_properties_add_user(ncl_mqtt_properties *props,
                                     const char *key, const char *value)
{
    if (props == NULL || key == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_strvec_push(&props->user_property_keys, key) != NCL_OK) {
        return NCL_ERR_NOMEM;
    }
    if (ncl_strvec_push(&props->user_property_values,
                        value != NULL ? value : "") != NCL_OK) {
        return NCL_ERR_NOMEM;
    }
    return NCL_OK;
}

const char *ncl_mqtt_properties_get_user(const ncl_mqtt_properties *props,
                                         const char *key)
{
    size_t i;
    if (props == NULL || key == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_strvec_len(&props->user_property_keys); i++) {
        if (strcmp(ncl_strvec_at(&props->user_property_keys, i), key) == 0) {
            return ncl_strvec_at(&props->user_property_values, i);
        }
    }
    return NULL;
}

bool ncl_mqtt_properties_has_user(const ncl_mqtt_properties *props,
                                  const char *key)
{
    return ncl_mqtt_properties_get_user(props, key) != NULL;
}

ncl_err ncl_mqtt_properties_write(const ncl_mqtt_properties *props,
                                  ncl_strbuf *out)
{
    ncl_strbuf body;
    unsigned char length_bytes[4];
    size_t length_len;
    size_t i;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_strbuf_init(&body);

#define NCL_PROP_BYTE(id, present, value)                                     \
    do {                                                                      \
        if (present) {                                                        \
            ncl_mqtt_write_property_id(&body, (id));                          \
            ncl_strbuf_putc(&body, (char)(value));                            \
        }                                                                     \
    } while (0)

    if (props != NULL) {
    NCL_PROP_BYTE(NCL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR,
                  props->has_payload_format_indicator,
                  props->payload_format_indicator);

    if (props->has_message_expiry_interval) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL);
        ncl_mqtt_write_u32(&body, props->message_expiry_interval);
    }
    if (props->content_type != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_CONTENT_TYPE);
        ncl_mqtt_write_string(&body, props->content_type);
    }
    if (props->response_topic != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_RESPONSE_TOPIC);
        ncl_mqtt_write_string(&body, props->response_topic);
    }
    if (props->correlation_data.data != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_CORRELATION_DATA);
        ncl_mqtt_write_binary(&body, props->correlation_data.data,
                              props->correlation_data.len);
    }
    if (props->has_subscription_identifier) {
        unsigned char bytes[4];
        size_t n;
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER);
        n = ncl_mqtt_varint_encode(props->subscription_identifier, bytes);
        ncl_strbuf_append(&body, (const char *)bytes, n);
    }
    if (props->has_session_expiry_interval) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_SESSION_EXPIRY_INTERVAL);
        ncl_mqtt_write_u32(&body, props->session_expiry_interval);
    }
    if (props->assigned_client_identifier != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_ASSIGNED_CLIENT_IDENTIFIER);
        ncl_mqtt_write_string(&body, props->assigned_client_identifier);
    }
    if (props->has_server_keep_alive) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_SERVER_KEEP_ALIVE);
        ncl_mqtt_write_u16(&body, props->server_keep_alive);
    }
    if (props->authentication_method != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_AUTHENTICATION_METHOD);
        ncl_mqtt_write_string(&body, props->authentication_method);
    }
    if (props->authentication_data.data != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_AUTHENTICATION_DATA);
        ncl_mqtt_write_binary(&body, props->authentication_data.data,
                              props->authentication_data.len);
    }
    NCL_PROP_BYTE(NCL_MQTT_PROP_REQUEST_PROBLEM_INFORMATION,
                  props->has_request_problem_information,
                  props->request_problem_information);
    if (props->has_will_delay_interval) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_WILL_DELAY_INTERVAL);
        ncl_mqtt_write_u32(&body, props->will_delay_interval);
    }
    NCL_PROP_BYTE(NCL_MQTT_PROP_REQUEST_RESPONSE_INFORMATION,
                  props->has_request_response_information,
                  props->request_response_information);
    if (props->response_information != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_RESPONSE_INFORMATION);
        ncl_mqtt_write_string(&body, props->response_information);
    }
    if (props->server_reference != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_SERVER_REFERENCE);
        ncl_mqtt_write_string(&body, props->server_reference);
    }
    if (props->reason_string != NULL) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_REASON_STRING);
        ncl_mqtt_write_string(&body, props->reason_string);
    }
    if (props->has_receive_maximum) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_RECEIVE_MAXIMUM);
        ncl_mqtt_write_u16(&body, props->receive_maximum);
    }
    if (props->has_topic_alias_maximum) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_TOPIC_ALIAS_MAXIMUM);
        ncl_mqtt_write_u16(&body, props->topic_alias_maximum);
    }
    if (props->has_topic_alias) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_TOPIC_ALIAS);
        ncl_mqtt_write_u16(&body, props->topic_alias);
    }
    NCL_PROP_BYTE(NCL_MQTT_PROP_MAXIMUM_QOS, props->has_maximum_qos,
                  props->maximum_qos);
    NCL_PROP_BYTE(NCL_MQTT_PROP_RETAIN_AVAILABLE, props->has_retain_available,
                  props->retain_available);

    for (i = 0; i < ncl_strvec_len(&props->user_property_keys); i++) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_USER_PROPERTY);
        ncl_mqtt_write_string(&body, ncl_strvec_at(&props->user_property_keys, i));
        ncl_mqtt_write_string(&body, ncl_strvec_at(&props->user_property_values, i));
    }

    if (props->has_maximum_packet_size) {
        ncl_mqtt_write_property_id(&body, NCL_MQTT_PROP_MAXIMUM_PACKET_SIZE);
        ncl_mqtt_write_u32(&body, props->maximum_packet_size);
    }
    NCL_PROP_BYTE(NCL_MQTT_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE,
                  props->has_wildcard_subscription_available,
                  props->wildcard_subscription_available);
    NCL_PROP_BYTE(NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE,
                  props->has_subscription_identifier_available,
                  props->subscription_identifier_available);
    NCL_PROP_BYTE(NCL_MQTT_PROP_SHARED_SUBSCRIPTION_AVAILABLE,
                  props->has_shared_subscription_available,
                  props->shared_subscription_available);
    } /* props != NULL */

#undef NCL_PROP_BYTE

    length_len = ncl_mqtt_varint_encode((uint32_t)body.len, length_bytes);
    if (ncl_strbuf_append(out, (const char *)length_bytes, length_len) != NCL_OK ||
        ncl_strbuf_append(out, body.data, body.len) != NCL_OK) {
        ncl_strbuf_free(&body);
        return NCL_ERR_NOMEM;
    }
    ncl_strbuf_free(&body);
    return NCL_OK;
}

bool ncl_mqtt_properties_block_is_empty(const ncl_strbuf *block)
{
    if (block == NULL) {
        return true;
    }
    /* An empty set is encoded as the single length byte 0x00. */
    return block->len == 1 && block->data != NULL && block->data[0] == 0;
}

/** Fixed payload length of a property, or -1 when it is variable. */
static int ncl_mqtt_property_fixed_size(uint8_t id)
{
    switch (id) {
    case NCL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:
    case NCL_MQTT_PROP_REQUEST_PROBLEM_INFORMATION:
    case NCL_MQTT_PROP_REQUEST_RESPONSE_INFORMATION:
    case NCL_MQTT_PROP_MAXIMUM_QOS:
    case NCL_MQTT_PROP_RETAIN_AVAILABLE:
    case NCL_MQTT_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE:
    case NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
    case NCL_MQTT_PROP_SHARED_SUBSCRIPTION_AVAILABLE:
        return 1;
    case NCL_MQTT_PROP_SERVER_KEEP_ALIVE:
    case NCL_MQTT_PROP_RECEIVE_MAXIMUM:
    case NCL_MQTT_PROP_TOPIC_ALIAS_MAXIMUM:
    case NCL_MQTT_PROP_TOPIC_ALIAS:
        return 2;
    case NCL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:
    case NCL_MQTT_PROP_SESSION_EXPIRY_INTERVAL:
    case NCL_MQTT_PROP_WILL_DELAY_INTERVAL:
    case NCL_MQTT_PROP_MAXIMUM_PACKET_SIZE:
        return 4;
    default:
        return -1;
    }
}

ncl_err ncl_mqtt_properties_read(const unsigned char *data, size_t len,
                                 ncl_mqtt_properties *props, size_t *consumed)
{
    ncl_mqtt_reader reader;
    uint32_t total;
    size_t used;
    size_t end;

    if (props == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (consumed != NULL) {
        *consumed = 0;
    }
    ncl_mqtt_properties_init(props);

    if (len == 0) {
        return NCL_OK;
    }
    used = ncl_mqtt_varint_decode(data, len, &total);
    if (used == 0) {
        return NCL_ERR_PARSE;
    }
    if (used + total > len) {
        return NCL_ERR_PARSE;
    }
    ncl_mqtt_reader_init(&reader, data + used, total);
    end = used + total;

    while (reader.pos < reader.len) {
        uint8_t id = ncl_mqtt_read_u8(&reader);
        if (reader.truncated) {
            ncl_mqtt_properties_free(props);
            return NCL_ERR_PARSE;
        }
        switch (id) {
        case NCL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:
            props->payload_format_indicator = ncl_mqtt_read_u8(&reader);
            props->has_payload_format_indicator = true;
            break;
        case NCL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:
            props->message_expiry_interval = ncl_mqtt_read_u32(&reader);
            props->has_message_expiry_interval = true;
            break;
        case NCL_MQTT_PROP_CONTENT_TYPE:
            ncl_mem_free(props->content_type);
            props->content_type = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_RESPONSE_TOPIC:
            ncl_mem_free(props->response_topic);
            props->response_topic = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_CORRELATION_DATA:
            ncl_buffer_free(&props->correlation_data);
            if (ncl_mqtt_read_binary(&reader, &props->correlation_data) != NCL_OK) {
                ncl_mqtt_properties_free(props);
                return NCL_ERR_PARSE;
            }
            break;
        case NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER:
            props->subscription_identifier = ncl_mqtt_read_varint(&reader);
            props->has_subscription_identifier = true;
            break;
        case NCL_MQTT_PROP_SESSION_EXPIRY_INTERVAL:
            props->session_expiry_interval = ncl_mqtt_read_u32(&reader);
            props->has_session_expiry_interval = true;
            break;
        case NCL_MQTT_PROP_ASSIGNED_CLIENT_IDENTIFIER:
            ncl_mem_free(props->assigned_client_identifier);
            props->assigned_client_identifier = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_SERVER_KEEP_ALIVE:
            props->server_keep_alive = ncl_mqtt_read_u16(&reader);
            props->has_server_keep_alive = true;
            break;
        case NCL_MQTT_PROP_AUTHENTICATION_METHOD:
            ncl_mem_free(props->authentication_method);
            props->authentication_method = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_AUTHENTICATION_DATA:
            ncl_buffer_free(&props->authentication_data);
            if (ncl_mqtt_read_binary(&reader, &props->authentication_data) != NCL_OK) {
                ncl_mqtt_properties_free(props);
                return NCL_ERR_PARSE;
            }
            break;
        case NCL_MQTT_PROP_REQUEST_PROBLEM_INFORMATION:
            props->request_problem_information = ncl_mqtt_read_u8(&reader);
            props->has_request_problem_information = true;
            break;
        case NCL_MQTT_PROP_WILL_DELAY_INTERVAL:
            props->will_delay_interval = ncl_mqtt_read_u32(&reader);
            props->has_will_delay_interval = true;
            break;
        case NCL_MQTT_PROP_REQUEST_RESPONSE_INFORMATION:
            props->request_response_information = ncl_mqtt_read_u8(&reader);
            props->has_request_response_information = true;
            break;
        case NCL_MQTT_PROP_RESPONSE_INFORMATION:
            ncl_mem_free(props->response_information);
            props->response_information = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_SERVER_REFERENCE:
            ncl_mem_free(props->server_reference);
            props->server_reference = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_REASON_STRING:
            ncl_mem_free(props->reason_string);
            props->reason_string = ncl_mqtt_read_string(&reader);
            break;
        case NCL_MQTT_PROP_RECEIVE_MAXIMUM:
            props->receive_maximum = ncl_mqtt_read_u16(&reader);
            props->has_receive_maximum = true;
            break;
        case NCL_MQTT_PROP_TOPIC_ALIAS_MAXIMUM:
            props->topic_alias_maximum = ncl_mqtt_read_u16(&reader);
            props->has_topic_alias_maximum = true;
            break;
        case NCL_MQTT_PROP_TOPIC_ALIAS:
            props->topic_alias = ncl_mqtt_read_u16(&reader);
            props->has_topic_alias = true;
            break;
        case NCL_MQTT_PROP_MAXIMUM_QOS:
            props->maximum_qos = ncl_mqtt_read_u8(&reader);
            props->has_maximum_qos = true;
            break;
        case NCL_MQTT_PROP_RETAIN_AVAILABLE:
            props->retain_available = ncl_mqtt_read_u8(&reader);
            props->has_retain_available = true;
            break;
        case NCL_MQTT_PROP_USER_PROPERTY: {
            char *key = ncl_mqtt_read_string(&reader);
            char *value = reader.truncated ? NULL : ncl_mqtt_read_string(&reader);
            if (key == NULL || value == NULL) {
                ncl_mem_free(key);
                ncl_mem_free(value);
                ncl_mqtt_properties_free(props);
                return NCL_ERR_PARSE;
            }
            ncl_mqtt_properties_add_user(props, key, value);
            ncl_mem_free(key);
            ncl_mem_free(value);
            break;
        }
        case NCL_MQTT_PROP_MAXIMUM_PACKET_SIZE:
            props->maximum_packet_size = ncl_mqtt_read_u32(&reader);
            props->has_maximum_packet_size = true;
            break;
        case NCL_MQTT_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE:
            props->wildcard_subscription_available = ncl_mqtt_read_u8(&reader);
            props->has_wildcard_subscription_available = true;
            break;
        case NCL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
            props->subscription_identifier_available = ncl_mqtt_read_u8(&reader);
            props->has_subscription_identifier_available = true;
            break;
        case NCL_MQTT_PROP_SHARED_SUBSCRIPTION_AVAILABLE:
            props->shared_subscription_available = ncl_mqtt_read_u8(&reader);
            props->has_shared_subscription_available = true;
            break;
        default: {
            /* Unknown property: skip by its fixed or prefixed length so that
             * newer brokers do not break the connection. */
            int fixed = ncl_mqtt_property_fixed_size(id);
            if (fixed > 0) {
                reader.pos += (size_t)fixed;
            } else if (id == NCL_MQTT_PROP_CONTENT_TYPE ||
                       id == NCL_MQTT_PROP_RESPONSE_TOPIC ||
                       id == NCL_MQTT_PROP_ASSIGNED_CLIENT_IDENTIFIER ||
                       id == NCL_MQTT_PROP_AUTHENTICATION_METHOD ||
                       id == NCL_MQTT_PROP_RESPONSE_INFORMATION ||
                       id == NCL_MQTT_PROP_SERVER_REFERENCE ||
                       id == NCL_MQTT_PROP_REASON_STRING) {
                uint16_t skip = ncl_mqtt_read_u16(&reader);
                reader.pos += skip;
            } else if (id == NCL_MQTT_PROP_CORRELATION_DATA ||
                       id == NCL_MQTT_PROP_AUTHENTICATION_DATA) {
                uint16_t skip = ncl_mqtt_read_u16(&reader);
                reader.pos += skip;
            } else {
                ncl_log_warn("MQTT: 未知属性 0x%02X，无法跳过", id);
                ncl_mqtt_properties_free(props);
                return NCL_ERR_PARSE;
            }
            break;
        }
        }
        if (reader.truncated || reader.pos > reader.len) {
            ncl_mqtt_properties_free(props);
            return NCL_ERR_PARSE;
        }
    }

    if (consumed != NULL) {
        *consumed = end;
    }
    return NCL_OK;
}

/* ============================================================== CONNECT === */

ncl_err ncl_mqtt_encode_connect(const ncl_mqtt_connect_options *options,
                                ncl_buffer *out)
{
    ncl_strbuf body;
    ncl_strbuf props;
    uint8_t flags = 0;
    ncl_err rc;

    if (options == NULL || options->client_id == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    out->data = NULL;
    out->len = 0;

    if (options->username != NULL) {
        flags |= 0x80;
    }
    if (options->password != NULL) {
        flags |= 0x40;
    }
    if (options->will_topic != NULL) {
        flags |= 0x04;
        flags |= (uint8_t)((options->will_qos & 0x03) << 3);
        if (options->will_retain) {
            flags |= 0x20;
        }
    }
    if (options->clean_start) {
        flags |= 0x02;
    }

    ncl_strbuf_init(&props);
    rc = ncl_mqtt_properties_write(options->properties, &props);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&props);
        return rc;
    }

    ncl_strbuf_init(&body);
    ncl_mqtt_write_string(&body, NCL_MQTT_PROTOCOL_NAME);
    ncl_strbuf_putc(&body, (char)NCL_MQTT_PROTOCOL_LEVEL);
    ncl_strbuf_putc(&body, (char)flags);
    ncl_mqtt_write_u16(&body, options->keep_alive_seconds);

    /* properties: already length prefixed by the writer */
    ncl_strbuf_append(&body, props.data, props.len);
    ncl_strbuf_free(&props);

    ncl_mqtt_write_string(&body, options->client_id);

    if (options->will_topic != NULL) {
        ncl_strbuf will_props;
        ncl_strbuf_init(&will_props);
        rc = ncl_mqtt_properties_write(options->will_properties, &will_props);
        if (rc != NCL_OK) {
            ncl_strbuf_free(&will_props);
            ncl_strbuf_free(&body);
            return rc;
        }
        ncl_strbuf_append(&body, will_props.data, will_props.len);
        ncl_strbuf_free(&will_props);
        ncl_mqtt_write_string(&body, options->will_topic);
        ncl_mqtt_write_binary(&body, options->will_payload,
                              options->will_payload_len);
    }
    if (options->username != NULL) {
        ncl_mqtt_write_string(&body, options->username);
    }
    if (options->password != NULL) {
        ncl_mqtt_write_binary(&body, (const unsigned char *)options->password,
                              strlen(options->password));
    }

    rc = ncl_mqtt_frame(0x10, &body, out);
    ncl_strbuf_free(&body);
    return rc;
}

void ncl_mqtt_connack_free(ncl_mqtt_connack *connack)
{
    if (connack == NULL) {
        return;
    }
    ncl_mqtt_properties_free(&connack->properties);
    memset(connack, 0, sizeof(*connack));
}

ncl_err ncl_mqtt_decode_connack(const unsigned char *body, size_t len,
                                ncl_mqtt_connack *out)
{
    ncl_mqtt_reader reader;
    uint8_t ack_flags;
    size_t consumed = 0;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    ncl_mqtt_properties_init(&out->properties);

    if (len < 3) {
        return NCL_ERR_PARSE;
    }
    ncl_mqtt_reader_init(&reader, body, len);
    ack_flags = ncl_mqtt_read_u8(&reader);
    out->session_present = (ack_flags & 0x01) != 0;
    out->reason_code = ncl_mqtt_read_u8(&reader);

    if (reader.pos < reader.len) {
        ncl_err rc = ncl_mqtt_properties_read(body + reader.pos,
                                             len - reader.pos,
                                             &out->properties, &consumed);
        if (rc != NCL_OK) {
            return rc;
        }
    }
    return NCL_OK;
}

/* ============================================================== PUBLISH === */

ncl_err ncl_mqtt_encode_publish(const char *topic,
                                const unsigned char *payload, size_t payload_len,
                                int qos, bool retain, bool duplicate,
                                uint16_t packet_id,
                                const ncl_mqtt_properties *properties,
                                ncl_buffer *out)
{
    ncl_strbuf body;
    ncl_strbuf props;
    uint8_t header = 0x30;
    ncl_err rc;

    if (topic == NULL || out == NULL || qos < 0 || qos > 2) {
        return NCL_ERR_INVALID_ARG;
    }
    out->data = NULL;
    out->len = 0;

    if (duplicate) {
        header |= 0x08;
    }
    header |= (uint8_t)((qos & 0x03) << 1);
    if (retain) {
        header |= 0x01;
    }

    ncl_strbuf_init(&props);
    rc = ncl_mqtt_properties_write(properties, &props);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&props);
        return rc;
    }

    ncl_strbuf_init(&body);
    ncl_mqtt_write_string(&body, topic);
    if (qos > 0) {
        ncl_mqtt_write_u16(&body, packet_id);
    }
    ncl_strbuf_append(&body, props.data, props.len);
    ncl_strbuf_free(&props);

    if (payload_len > 0) {
        ncl_strbuf_append(&body, (const char *)payload, payload_len);
    }

    rc = ncl_mqtt_frame(header, &body, out);
    ncl_strbuf_free(&body);
    return rc;
}

void ncl_mqtt_publish_free(ncl_mqtt_publish *publish)
{
    if (publish == NULL) {
        return;
    }
    ncl_mem_free(publish->topic);
    ncl_mqtt_properties_free(&publish->properties);
    memset(publish, 0, sizeof(*publish));
}

ncl_err ncl_mqtt_decode_publish(uint8_t header_flags, const unsigned char *body,
                                size_t len, ncl_mqtt_publish *out)
{
    ncl_mqtt_reader reader;
    size_t consumed = 0;
    uint32_t props_len;
    size_t used;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    ncl_mqtt_properties_init(&out->properties);

    out->duplicate = (header_flags & 0x08) != 0;
    out->qos = (header_flags & 0x06) >> 1;
    out->retain = (header_flags & 0x01) != 0;

    ncl_mqtt_reader_init(&reader, body, len);
    out->topic = ncl_mqtt_read_string(&reader);
    if (out->topic == NULL) {
        ncl_mqtt_publish_free(out);
        return NCL_ERR_PARSE;
    }
    if (out->qos > 0) {
        out->packet_id = ncl_mqtt_read_u16(&reader);
        if (reader.truncated) {
            ncl_mqtt_publish_free(out);
            return NCL_ERR_PARSE;
        }
    }

    if (reader.pos >= reader.len) {
        ncl_mqtt_publish_free(out);
        return NCL_ERR_PARSE;
    }
    used = ncl_mqtt_varint_decode(body + reader.pos, reader.len - reader.pos,
                                  &props_len);
    if (used == 0 || reader.pos + used + props_len > reader.len) {
        ncl_mqtt_publish_free(out);
        return NCL_ERR_PARSE;
    }
    {
        ncl_err rc = ncl_mqtt_properties_read(body + reader.pos,
                                             reader.len - reader.pos,
                                             &out->properties, &consumed);
        if (rc != NCL_OK) {
            ncl_mqtt_publish_free(out);
            return rc;
        }
    }
    reader.pos += consumed;

    out->payload = body + reader.pos;
    out->payload_len = len - reader.pos;
    return NCL_OK;
}

/* ================================== PUBACK / PUBREC / PUBREL / PUBCOMP ==== */

ncl_err ncl_mqtt_encode_ack(ncl_mqtt_packet_type type, uint16_t packet_id,
                            uint8_t reason_code, ncl_buffer *out)
{
    ncl_strbuf body;
    uint8_t header;
    ncl_err rc;

    switch (type) {
    case NCL_MQTT_PKT_PUBACK: header = 0x40; break;
    case NCL_MQTT_PKT_PUBREC: header = 0x50; break;
    case NCL_MQTT_PKT_PUBREL: header = 0x62; break;
    case NCL_MQTT_PKT_PUBCOMP: header = 0x70; break;
    default:
        return NCL_ERR_INVALID_ARG;
    }

    ncl_strbuf_init(&body);
    ncl_mqtt_write_u16(&body, packet_id);
    if (reason_code != NCL_MQTT_REASON_SUCCESS) {
        /* Non-zero reason: emit the reason code and an empty property set. */
        ncl_strbuf_putc(&body, (char)reason_code);
        ncl_strbuf_putc(&body, 0);
    }
    rc = ncl_mqtt_frame(header, &body, out);
    ncl_strbuf_free(&body);
    return rc;
}

ncl_err ncl_mqtt_decode_ack(const unsigned char *body, size_t len,
                            uint16_t *packet_id, uint8_t *reason_code)
{
    ncl_mqtt_reader reader;

    if (body == NULL || len < 2) {
        return NCL_ERR_PARSE;
    }
    ncl_mqtt_reader_init(&reader, body, len);
    if (packet_id != NULL) {
        *packet_id = ncl_mqtt_read_u16(&reader);
    } else {
        ncl_mqtt_read_u16(&reader);
    }
    if (reason_code != NULL) {
        *reason_code = reader.pos < reader.len ? ncl_mqtt_read_u8(&reader)
                                               : NCL_MQTT_REASON_SUCCESS;
    }
    return NCL_OK;
}

/* =========================================== SUBSCRIBE / UNSUBSCRIBE ====== */

static ncl_err ncl_mqtt_encode_subscription(ncl_mqtt_packet_type type,
                                            uint8_t header, uint16_t packet_id,
                                            const char *topic_filter, int qos,
                                            const ncl_mqtt_properties *properties,
                                            ncl_buffer *out)
{
    ncl_strbuf body;
    ncl_strbuf props;
    ncl_err rc;

    (void)type;
    if (topic_filter == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }

    ncl_strbuf_init(&props);
    rc = ncl_mqtt_properties_write(properties, &props);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&props);
        return rc;
    }

    ncl_strbuf_init(&body);
    ncl_mqtt_write_u16(&body, packet_id);
    ncl_strbuf_append(&body, props.data, props.len);
    ncl_strbuf_free(&props);
    ncl_mqtt_write_string(&body, topic_filter);
    if (header == 0x82) { /* SUBSCRIBE carries subscription options */
        ncl_strbuf_putc(&body, (char)(qos & 0x03));
    }

    rc = ncl_mqtt_frame(header, &body, out);
    ncl_strbuf_free(&body);
    return rc;
}

ncl_err ncl_mqtt_encode_subscribe(uint16_t packet_id, const char *topic_filter,
                                  int qos, const ncl_mqtt_properties *properties,
                                  ncl_buffer *out)
{
    return ncl_mqtt_encode_subscription(NCL_MQTT_PKT_SUBSCRIBE, 0x82, packet_id,
                                        topic_filter, qos, properties, out);
}

ncl_err ncl_mqtt_encode_unsubscribe(uint16_t packet_id, const char *topic_filter,
                                    const ncl_mqtt_properties *properties,
                                    ncl_buffer *out)
{
    return ncl_mqtt_encode_subscription(NCL_MQTT_PKT_UNSUBSCRIBE, 0xA2, packet_id,
                                        topic_filter, 0, properties, out);
}

void ncl_mqtt_suback_free(ncl_mqtt_suback *suback)
{
    if (suback == NULL) {
        return;
    }
    ncl_mqtt_properties_free(&suback->properties);
    ncl_mem_free(suback->reason_codes);
    memset(suback, 0, sizeof(*suback));
}

ncl_err ncl_mqtt_decode_suback(const unsigned char *body, size_t len,
                               ncl_mqtt_suback *out)
{
    ncl_mqtt_reader reader;
    size_t consumed = 0;
    size_t remaining;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    ncl_mqtt_properties_init(&out->properties);

    if (len < 4) {
        return NCL_ERR_PARSE;
    }
    ncl_mqtt_reader_init(&reader, body, len);
    out->packet_id = ncl_mqtt_read_u16(&reader);
    if (ncl_mqtt_properties_read(body + reader.pos, len - reader.pos,
                                 &out->properties, &consumed) != NCL_OK) {
        ncl_mqtt_suback_free(out);
        return NCL_ERR_PARSE;
    }
    reader.pos += consumed;

    remaining = reader.len - reader.pos;
    if (remaining > 0) {
        out->reason_codes = (uint8_t *)ncl_mem_alloc(remaining);
        if (out->reason_codes == NULL) {
            ncl_mqtt_suback_free(out);
            return NCL_ERR_NOMEM;
        }
        memcpy(out->reason_codes, body + reader.pos, remaining);
        out->reason_code_count = remaining;
    }
    return NCL_OK;
}

/* ================================== PINGREQ / PINGRESP / DISCONNECT ======= */

ncl_err ncl_mqtt_encode_ping(bool response, ncl_buffer *out)
{
    ncl_strbuf body;
    ncl_err rc;
    ncl_strbuf_init(&body);
    rc = ncl_mqtt_frame(response ? 0xD0 : 0xC0, &body, out);
    ncl_strbuf_free(&body);
    return rc;
}

ncl_err ncl_mqtt_encode_disconnect(uint8_t reason_code,
                                   const ncl_mqtt_properties *properties,
                                   ncl_buffer *out)
{
    ncl_strbuf body;
    ncl_strbuf props;
    ncl_err rc;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_strbuf_init(&props);
    rc = ncl_mqtt_properties_write(properties, &props);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&props);
        return rc;
    }

    ncl_strbuf_init(&body);
    if (reason_code != NCL_MQTT_REASON_SUCCESS ||
        !ncl_mqtt_properties_block_is_empty(&props)) {
        ncl_strbuf_putc(&body, (char)reason_code);
        ncl_strbuf_append(&body, props.data, props.len);
    }
    ncl_strbuf_free(&props);

    rc = ncl_mqtt_frame(0xE0, &body, out);
    ncl_strbuf_free(&body);
    return rc;
}

void ncl_mqtt_disconnect_free(ncl_mqtt_disconnect *disconnect)
{
    if (disconnect == NULL) {
        return;
    }
    ncl_mqtt_properties_free(&disconnect->properties);
    memset(disconnect, 0, sizeof(*disconnect));
}

ncl_err ncl_mqtt_decode_disconnect(const unsigned char *body, size_t len,
                                   ncl_mqtt_disconnect *out)
{
    size_t consumed = 0;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    ncl_mqtt_properties_init(&out->properties);
    out->reason_code = NCL_MQTT_REASON_SUCCESS;

    if (len == 0) {
        return NCL_OK;
    }
    out->reason_code = body[0];
    if (len > 1) {
        if (ncl_mqtt_properties_read(body + 1, len - 1, &out->properties,
                                     &consumed) != NCL_OK) {
            ncl_mqtt_disconnect_free(out);
            return NCL_ERR_PARSE;
        }
    }
    return NCL_OK;
}

/* ================================================== fixed header helpers == */

bool ncl_mqtt_peek_header(const unsigned char *data, size_t len,
                          ncl_mqtt_packet_type *type, uint8_t *flags,
                          uint32_t *remaining_length, size_t *header_len)
{
    uint32_t value = 0;
    size_t used;

    if (data == NULL || len < 2) {
        return false;
    }
    used = ncl_mqtt_varint_decode(data + 1, len - 1, &value);
    if (used == 0) {
        return false;
    }
    if (type != NULL) {
        *type = (ncl_mqtt_packet_type)((data[0] >> 4) & 0x0F);
    }
    if (flags != NULL) {
        *flags = (uint8_t)(data[0] & 0x0F);
    }
    if (remaining_length != NULL) {
        *remaining_length = value;
    }
    if (header_len != NULL) {
        *header_len = 1 + used;
    }
    return true;
}

const char *ncl_mqtt_packet_type_name(ncl_mqtt_packet_type type)
{
    switch (type) {
    case NCL_MQTT_PKT_CONNECT: return "CONNECT";
    case NCL_MQTT_PKT_CONNACK: return "CONNACK";
    case NCL_MQTT_PKT_PUBLISH: return "PUBLISH";
    case NCL_MQTT_PKT_PUBACK: return "PUBACK";
    case NCL_MQTT_PKT_PUBREC: return "PUBREC";
    case NCL_MQTT_PKT_PUBREL: return "PUBREL";
    case NCL_MQTT_PKT_PUBCOMP: return "PUBCOMP";
    case NCL_MQTT_PKT_SUBSCRIBE: return "SUBSCRIBE";
    case NCL_MQTT_PKT_SUBACK: return "SUBACK";
    case NCL_MQTT_PKT_UNSUBSCRIBE: return "UNSUBSCRIBE";
    case NCL_MQTT_PKT_UNSUBACK: return "UNSUBACK";
    case NCL_MQTT_PKT_PINGREQ: return "PINGREQ";
    case NCL_MQTT_PKT_PINGRESP: return "PINGRESP";
    case NCL_MQTT_PKT_DISCONNECT: return "DISCONNECT";
    case NCL_MQTT_PKT_AUTH: return "AUTH";
    default: return "UNKNOWN";
    }
}
