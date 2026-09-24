/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * MQTT 5.0 packet codec tests.
 *
 * The encode cases assert exact byte sequences computed by hand from the OASIS
 * MQTT 5.0 specification, so that a regression cannot silently change the wire
 * format. The decode cases round trip those same packets.
 */
#include "ncl_test.h"

#include "nclink/ncl_mqtt.h"

static int bytes_equal(const ncl_buffer *buf, const unsigned char *expected,
                       size_t expected_len)
{
    if (buf->len != expected_len) {
        return 0;
    }
    return memcmp(buf->data, expected, expected_len) == 0;
}

static void dump(const ncl_buffer *buf)
{
    size_t i;
    printf("      got:");
    for (i = 0; i < buf->len; i++) {
        printf(" %02X", buf->data[i]);
    }
    printf("\n");
}

static void test_varint(void)
{
    unsigned char out[4];
    uint32_t value = 0;
    size_t n;

    NCL_TEST_CASE("variable byte integer encoding matches the spec table");
    n = ncl_mqtt_varint_encode(0, out);
    NCL_CHECK_EQ_INT(n, 1);
    NCL_CHECK_EQ_INT(out[0], 0x00);

    n = ncl_mqtt_varint_encode(127, out);
    NCL_CHECK_EQ_INT(n, 1);
    NCL_CHECK_EQ_INT(out[0], 0x7F);

    n = ncl_mqtt_varint_encode(128, out);
    NCL_CHECK_EQ_INT(n, 2);
    NCL_CHECK_EQ_INT(out[0], 0x80);
    NCL_CHECK_EQ_INT(out[1], 0x01);

    n = ncl_mqtt_varint_encode(16383, out);
    NCL_CHECK_EQ_INT(n, 2);
    NCL_CHECK_EQ_INT(out[0], 0xFF);
    NCL_CHECK_EQ_INT(out[1], 0x7F);

    n = ncl_mqtt_varint_encode(2097151, out);
    NCL_CHECK_EQ_INT(n, 3);
    NCL_CHECK_EQ_INT(out[0], 0xFF);
    NCL_CHECK_EQ_INT(out[1], 0xFF);
    NCL_CHECK_EQ_INT(out[2], 0x7F);

    n = ncl_mqtt_varint_encode(268435455, out);
    NCL_CHECK_EQ_INT(n, 4);
    NCL_CHECK_EQ_INT(out[3], 0x7F);

    NCL_TEST_CASE("variable byte integer decoding");
    NCL_CHECK_EQ_INT(ncl_mqtt_varint_decode((const unsigned char *)"\x80\x01", 2, &value), 2);
    NCL_CHECK_EQ_INT(value, 128);
    NCL_CHECK_EQ_INT(ncl_mqtt_varint_decode((const unsigned char *)"\xFF\xFF\xFF\x7F", 4, &value), 4);
    NCL_CHECK_EQ_INT(value, 268435455);
    NCL_CHECK_EQ_INT(ncl_mqtt_varint_decode((const unsigned char *)"\x80", 1, &value), 0);
}

static void test_connect(void)
{
    ncl_mqtt_connect_options options;
    ncl_mqtt_properties props;
    ncl_buffer out;
    static const unsigned char expected_plain[] = {
        0x10, 0x10,                                     /* CONNECT, 16 bytes */
        0x00, 0x04, 'M', 'Q', 'T', 'T',                 /* protocol name     */
        0x05,                                           /* protocol level 5  */
        0x02,                                           /* clean start       */
        0x00, 0x3C,                                     /* keep alive 60 s   */
        0x00,                                           /* no properties     */
        0x00, 0x03, 'c', 'i', 'd'                       /* client id         */
    };

    memset(&options, 0, sizeof(options));
    options.client_id = "cid";
    options.clean_start = true;
    options.keep_alive_seconds = 60;

    NCL_TEST_CASE("CONNECT minimal packet");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_connect(&options, &out), NCL_OK);
    if (!bytes_equal(&out, expected_plain, sizeof(expected_plain))) {
        dump(&out);
    }
    NCL_CHECK(bytes_equal(&out, expected_plain, sizeof(expected_plain)));
    ncl_buffer_free(&out);

    NCL_TEST_CASE("CONNECT with username, password and version=2.0 user property");
    ncl_mqtt_properties_init(&props);
    ncl_mqtt_properties_add_user(&props, "version", "2.0");
    options.username = "admin";
    options.password = "123456";
    options.properties = &props;
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_connect(&options, &out), NCL_OK);
    /* flags: username|password|clean start = 0xC2
     * body layout: [0..5] protocol name, [6] level, [7] flags,
     *              [8..9] keep alive, [10] property length, [11..] properties */
    NCL_CHECK_EQ_INT(out.data[0], 0x10);
    NCL_CHECK_EQ_INT(out.data[2 + 6], 0x05);
    NCL_CHECK_EQ_INT(out.data[2 + 7], 0xC2);
    NCL_CHECK_EQ_INT(out.data[2 + 10], 0x0F);
    NCL_CHECK_EQ_INT(out.data[2 + 11], 0x26);
    NCL_CHECK_EQ_INT(out.data[2 + 12], 0x00);
    NCL_CHECK_EQ_INT(out.data[2 + 13], 0x07);

    NCL_TEST_CASE("CONNECT packet has a well formed fixed header");
    {
        ncl_mqtt_packet_type type;
        uint8_t flags;
        uint32_t remaining;
        size_t header_len;
        NCL_CHECK(ncl_mqtt_peek_header(out.data, out.len, &type, &flags,
                                       &remaining, &header_len));
        NCL_CHECK_EQ_INT(type, NCL_MQTT_PKT_CONNECT);
        NCL_CHECK_EQ_INT(flags, 0);
        NCL_CHECK_EQ_INT(header_len + remaining, out.len);
    }
    ncl_buffer_free(&out);
    ncl_mqtt_properties_free(&props);
}

static void test_publish(void)
{
    ncl_buffer out;
    ncl_mqtt_properties props;
    ncl_mqtt_publish decoded;
    static const unsigned char expected_qos0[] = {
        0x30, 0x07, 0x00, 0x02, '/', 'a', 0x00, 'h', 'i'
    };

    NCL_TEST_CASE("PUBLISH QoS 0 without properties");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_publish("/a", (const unsigned char *)"hi", 2,
                                             0, false, false, 0, NULL, &out),
                     NCL_OK);
    if (!bytes_equal(&out, expected_qos0, sizeof(expected_qos0))) {
        dump(&out);
    }
    NCL_CHECK(bytes_equal(&out, expected_qos0, sizeof(expected_qos0)));
    ncl_buffer_free(&out);

    NCL_TEST_CASE("PUBLISH QoS 2 carries a packet identifier");
    {
        ncl_mqtt_packet_type type;
        uint8_t flags = 0;
        uint32_t remaining = 0;
        size_t header_len = 0;
        NCL_CHECK_EQ_INT(ncl_mqtt_encode_publish("Query/Request/V1",
                                                 (const unsigned char *)"{}", 2,
                                                 2, false, false, 7, NULL, &out),
                         NCL_OK);
        NCL_CHECK(ncl_mqtt_peek_header(out.data, out.len, &type, &flags,
                                       &remaining, &header_len));
        NCL_CHECK_EQ_INT(type, NCL_MQTT_PKT_PUBLISH);
        NCL_CHECK_EQ_INT(flags, 0x04); /* QoS 2 */
        NCL_CHECK_EQ_INT(out.data[header_len + 2 + 16], 0x00);
        NCL_CHECK_EQ_INT(out.data[header_len + 2 + 17], 0x07);
        ncl_buffer_free(&out);
    }

    NCL_TEST_CASE("PUBLISH with the NC-Link version user property round trips");
    ncl_mqtt_properties_init(&props);
    ncl_mqtt_properties_add_user(&props, "version", "2.0");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_publish("Query/Request/V203243111F",
                                             (const unsigned char *)"{\"@id\":\"m1\"}",
                                             12, 2, false, false, 3, &props, &out),
                     NCL_OK);
    {
        ncl_mqtt_packet_type type;
        uint8_t flags = 0;
        uint32_t remaining = 0;
        size_t header_len = 0;
        NCL_CHECK(ncl_mqtt_peek_header(out.data, out.len, &type, &flags,
                                       &remaining, &header_len));
        NCL_CHECK_EQ_INT(ncl_mqtt_decode_publish(flags, out.data + header_len,
                                                remaining, &decoded),
                         NCL_OK);
        NCL_CHECK_EQ_STR(decoded.topic, "Query/Request/V203243111F");
        NCL_CHECK_EQ_INT(decoded.qos, 2);
        NCL_CHECK_EQ_INT(decoded.packet_id, 3);
        NCL_CHECK_EQ_INT(decoded.payload_len, 12);
        NCL_CHECK(memcmp(decoded.payload, "{\"@id\":\"m1\"}", 12) == 0);
        NCL_CHECK_EQ_STR(ncl_mqtt_properties_get_user(&decoded.properties, "version"),
                         "2.0");
        ncl_mqtt_publish_free(&decoded);
    }
    ncl_buffer_free(&out);
    ncl_mqtt_properties_free(&props);
}

static void test_connack(void)
{
    /* CONNACK without properties: flags, reason code, empty property block. */
    static const unsigned char empty_body[] = {0x00, 0x00, 0x00};
    /* CONNACK advertising an assigned client identifier:
     *   0x12 = assigned client identifier, 0x000B = length, then 11 characters
     *   the property block is 1 (length byte) + 1 + 2 + 11 = 15 bytes. */
    static const unsigned char assigned_body[] = {
        0x00, 0x00,
        0x0E, 0x12, 0x00, 0x0B,
        'a', 'u', 't', 'o', '-', 'c', 'l', 'i', 'e', 'n', 't'
    };
    ncl_mqtt_connack connack;

    NCL_TEST_CASE("CONNACK flags, reason code and properties");
    NCL_CHECK_EQ_INT(ncl_mqtt_decode_connack(empty_body, sizeof(empty_body),
                                             &connack),
                     NCL_OK);
    NCL_CHECK(connack.session_present == false);
    NCL_CHECK_EQ_INT(connack.reason_code, 0);
    NCL_CHECK_EQ_INT(ncl_strvec_len(&connack.properties.user_property_keys), 0);
    ncl_mqtt_connack_free(&connack);

    NCL_TEST_CASE("CONNACK assigned client identifier is decoded");
    NCL_CHECK_EQ_INT(ncl_mqtt_decode_connack(assigned_body,
                                             sizeof(assigned_body), &connack),
                     NCL_OK);
    NCL_CHECK_EQ_STR(connack.properties.assigned_client_identifier, "auto-client");
    ncl_mqtt_connack_free(&connack);

    NCL_TEST_CASE("truncated CONNACK is rejected");
    NCL_CHECK_EQ_INT(ncl_mqtt_decode_connack(empty_body, 2, &connack),
                     NCL_ERR_PARSE);
}

static void test_ack(void)
{
    ncl_buffer out;
    uint16_t packet_id = 0;
    uint8_t reason = 0xFF;

    NCL_TEST_CASE("PUBACK with the success reason code is two bytes of body");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBACK, 42,
                                         NCL_MQTT_REASON_SUCCESS, &out),
                     NCL_OK);
    NCL_CHECK_EQ_INT(out.len, 4);
    NCL_CHECK_EQ_INT(out.data[0], 0x40);
    NCL_CHECK_EQ_INT(out.data[1], 0x02);
    NCL_CHECK_EQ_INT(out.data[2], 0x00);
    NCL_CHECK_EQ_INT(out.data[3], 0x2A);
    NCL_CHECK_EQ_INT(ncl_mqtt_decode_ack(out.data + 2, 2, &packet_id, &reason),
                     NCL_OK);
    NCL_CHECK_EQ_INT(packet_id, 42);
    NCL_CHECK_EQ_INT(reason, NCL_MQTT_REASON_SUCCESS);
    ncl_buffer_free(&out);

    NCL_TEST_CASE("non-zero reason codes carry the reason and a property block");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBREC, 5,
                                         NCL_MQTT_REASON_UNSPECIFIED_ERROR, &out),
                     NCL_OK);
    NCL_CHECK_EQ_INT(out.len, 6);
    NCL_CHECK_EQ_INT(out.data[1], 0x04);
    NCL_CHECK_EQ_INT(out.data[4], 0x80);
    NCL_CHECK_EQ_INT(out.data[5], 0x00);
    NCL_CHECK_EQ_INT(ncl_mqtt_decode_ack(out.data + 2, 4, &packet_id, &reason),
                     NCL_OK);
    NCL_CHECK_EQ_INT(packet_id, 5);
    NCL_CHECK_EQ_INT(reason, 0x80);
    ncl_buffer_free(&out);

    NCL_TEST_CASE("PUBREL uses the reserved 0x62 header");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_ack(NCL_MQTT_PKT_PUBREL, 1,
                                         NCL_MQTT_REASON_SUCCESS, &out),
                     NCL_OK);
    NCL_CHECK_EQ_INT(out.data[0], 0x62);
    ncl_buffer_free(&out);
}

static void test_subscribe(void)
{
    ncl_buffer out;
    ncl_mqtt_suback suback;
    static const unsigned char expected[] = {
        0x82, 0x17,                                /* SUBSCRIBE, 23 bytes */
        0x00, 0x01,                                /* packet id 1         */
        0x00,                                      /* no properties       */
        0x00, 0x11,                                /* topic length 17     */
        'Q','u','e','r','y','/','R','e','s','p','o','n','s','e','/','V','1',
        0x02                                       /* requested QoS 2     */
    };

    NCL_TEST_CASE("SUBSCRIBE byte layout");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_subscribe(1, "Query/Response/V1", 2, NULL,
                                               &out),
                     NCL_OK);
    if (!bytes_equal(&out, expected, sizeof(expected))) {
        dump(&out);
    }
    NCL_CHECK(bytes_equal(&out, expected, sizeof(expected)));
    ncl_buffer_free(&out);

    NCL_TEST_CASE("UNSUBSCRIBE omits the subscription options byte");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_unsubscribe(1, "Query/Response/V1", NULL,
                                                 &out),
                     NCL_OK);
    NCL_CHECK_EQ_INT(out.data[0], 0xA2);
    NCL_CHECK_EQ_INT(out.len, sizeof(expected) - 1);
    ncl_buffer_free(&out);

    NCL_TEST_CASE("SUBACK reason codes are decoded in order");
    {
        static const unsigned char body[] = {
            0x00, 0x01, 0x00, 0x02, 0x01
        };
        NCL_CHECK_EQ_INT(ncl_mqtt_decode_suback(body, sizeof(body), &suback),
                         NCL_OK);
        NCL_CHECK_EQ_INT(suback.packet_id, 1);
        NCL_CHECK_EQ_INT(suback.reason_code_count, 2);
        NCL_CHECK_EQ_INT(suback.reason_codes[0], 0x02);
        NCL_CHECK_EQ_INT(suback.reason_codes[1], 0x01);
        ncl_mqtt_suback_free(&suback);
    }
}

static void test_ping_and_disconnect(void)
{
    ncl_buffer out;
    ncl_mqtt_disconnect info;

    NCL_TEST_CASE("PINGREQ / PINGRESP are two byte packets");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_ping(false, &out), NCL_OK);
    NCL_CHECK_EQ_INT(out.len, 2);
    NCL_CHECK_EQ_INT(out.data[0], 0xC0);
    NCL_CHECK_EQ_INT(out.data[1], 0x00);
    ncl_buffer_free(&out);

    NCL_CHECK_EQ_INT(ncl_mqtt_encode_ping(true, &out), NCL_OK);
    NCL_CHECK_EQ_INT(out.data[0], 0xD0);
    ncl_buffer_free(&out);

    NCL_TEST_CASE("DISCONNECT normal form carries no body");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_disconnect(NCL_MQTT_REASON_SUCCESS, NULL, &out),
                     NCL_OK);
    NCL_CHECK_EQ_INT(out.len, 2);
    NCL_CHECK_EQ_INT(out.data[0], 0xE0);
    ncl_buffer_free(&out);

    NCL_TEST_CASE("DISCONNECT with a reason code round trips");
    NCL_CHECK_EQ_INT(ncl_mqtt_encode_disconnect(NCL_MQTT_REASON_SESSION_TAKEN_OVER,
                                                NULL, &out),
                     NCL_OK);
    NCL_CHECK_EQ_INT(out.data[0], 0xE0);
    NCL_CHECK_EQ_INT(out.data[1], 0x02);
    NCL_CHECK_EQ_INT(out.data[2], 0x8E);
    NCL_CHECK_EQ_INT(out.data[3], 0x00); /* empty property block */
    NCL_CHECK_EQ_INT(ncl_mqtt_decode_disconnect(out.data + 2, 2, &info), NCL_OK);
    NCL_CHECK_EQ_INT(info.reason_code, NCL_MQTT_REASON_SESSION_TAKEN_OVER);
    ncl_mqtt_disconnect_free(&info);
    ncl_buffer_free(&out);
}

static void test_properties_round_trip(void)
{
    ncl_mqtt_properties props;
    ncl_mqtt_properties parsed;
    ncl_strbuf buf;
    size_t consumed = 0;

    NCL_TEST_CASE("property block round trips including ordered user properties");
    ncl_mqtt_properties_init(&props);
    ncl_mqtt_properties_add_user(&props, "version", "2.0");
    ncl_mqtt_properties_add_user(&props, "sn", "V203243111F");
    props.has_session_expiry_interval = true;
    props.session_expiry_interval = 300;
    props.has_receive_maximum = true;
    props.receive_maximum = 20;
    props.content_type = ncl_strdup("application/json");

    ncl_strbuf_init(&buf);
    NCL_CHECK_EQ_INT(ncl_mqtt_properties_write(&props, &buf), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_mqtt_properties_read((const unsigned char *)buf.data,
                                              buf.len, &parsed, &consumed),
                     NCL_OK);
    NCL_CHECK_EQ_INT(consumed, buf.len);
    NCL_CHECK_EQ_STR(ncl_mqtt_properties_get_user(&parsed, "version"), "2.0");
    NCL_CHECK_EQ_STR(ncl_mqtt_properties_get_user(&parsed, "sn"), "V203243111F");
    NCL_CHECK(ncl_mqtt_properties_has_user(&parsed, "version"));
    NCL_CHECK(!ncl_mqtt_properties_has_user(&parsed, "absent"));
    NCL_CHECK_EQ_INT(parsed.session_expiry_interval, 300);
    NCL_CHECK_EQ_INT(parsed.receive_maximum, 20);
    NCL_CHECK_EQ_STR(parsed.content_type, "application/json");

    ncl_strbuf_free(&buf);
    ncl_mqtt_properties_free(&props);
    ncl_mqtt_properties_free(&parsed);

    NCL_TEST_CASE("a truncated property block is rejected");
    {
        static const unsigned char truncated[] = {
            0x05, 0x26, 0x00, 0x07, 'v'
        };
        NCL_CHECK_EQ_INT(ncl_mqtt_properties_read(truncated, sizeof(truncated),
                                                  &parsed, &consumed),
                         NCL_ERR_PARSE);
    }

    NCL_TEST_CASE("an empty property set encodes as a single zero byte");
    {
        ncl_strbuf empty;
        ncl_strbuf_init(&empty);
        NCL_CHECK_EQ_INT(ncl_mqtt_properties_write(NULL, &empty), NCL_OK);
        NCL_CHECK_EQ_INT(empty.len, 1);
        NCL_CHECK_EQ_INT(empty.data[0], 0x00);
        NCL_CHECK(ncl_mqtt_properties_block_is_empty(&empty));
        NCL_CHECK_EQ_INT(ncl_mqtt_properties_read((const unsigned char *)empty.data,
                                                  empty.len, &parsed, &consumed),
                         NCL_OK);
        ncl_mqtt_properties_free(&parsed);
        ncl_strbuf_free(&empty);
    }
}

NCL_TEST_MAIN_BEGIN()
    test_varint();
    test_connect();
    test_publish();
    test_connack();
    test_ack();
    test_subscribe();
    test_ping_and_disconnect();
    test_properties_round_trip();
NCL_TEST_MAIN_END()
