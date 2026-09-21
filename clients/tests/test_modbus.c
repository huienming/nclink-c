/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Modbus: byte level first (golden frames, CRC, byte order, exceptions), then a
 * real one - the test runs its own Modbus TCP slave on a loopback socket and
 * drives the driver against it, so framing, merging, retries and decoding are
 * all exercised over a wire rather than against a stub.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/clients/modbus.h"
#include "test_point_map.h"
#include "modbus/ncl_modbus_driver.h"

/* ============================================================== the byte == */

static void test_crc(void)
{
    /* The worked example of the Modbus spec: 01 04 02 FF FF, CRC bytes B8 80
     * on the wire, i.e. the value 0x80B8 with its low byte sent first. */
    static const uint8_t kFrame[] = {0x01, 0x04, 0x02, 0xFF, 0xFF};

    NCL_TEST_CASE("CRC16 is the reversed 0xA001 polynomial, low byte first");
    NCL_CHECK_EQ_INT(ncl_modbus_crc16(kFrame, sizeof(kFrame)), 0x80B8);
    NCL_CHECK_EQ_INT(ncl_modbus_crc16((const uint8_t *)"123456789", 9), 0x4B37);
}

static void test_tcp_frame(void)
{
    uint8_t pdu[8];
    uint8_t frame[32];
    size_t pdu_len;
    size_t frame_len;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;

    NCL_TEST_CASE("a Modbus TCP read request is MBAP + PDU");
    pdu_len = ncl_modbus_read_pdu(pdu, sizeof(pdu), NCL_MB_FC_READ_HOLDING, 0, 10);
    NCL_CHECK_EQ_INT(pdu_len, 5);
    frame_len = ncl_modbus_tcp_frame(frame, sizeof(frame), 0x1234, 0x01, pdu, pdu_len);
    NCL_CHECK_EQ_INT(frame_len, 12);
    {
        static const uint8_t kExpected[] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x06,
                                            0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};

        NCL_CHECK(memcmp(frame, kExpected, sizeof(kExpected)) == 0);
    }

    NCL_TEST_CASE("a Modbus TCP reply splits back into the PDU");
    {
        static const uint8_t kReply[] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x05,
                                         0x01, 0x03, 0x02, 0x00, 0x2A};

        NCL_CHECK_EQ_INT(ncl_modbus_tcp_split(kReply, sizeof(kReply), 0x1234,
                                              &reply, &reply_len, &frame_len),
                         NCL_OK);
        NCL_CHECK_EQ_INT(reply_len, 4);
        NCL_CHECK_EQ_INT(reply[0], 0x03);
        NCL_CHECK_EQ_INT(reply[3], 0x2A);
        NCL_CHECK_EQ_INT(ncl_modbus_tcp_frame(NULL, 0, 0, 0, NULL, 0), 0);
        /* a reply to an older request is a protocol error, not data */
        NCL_CHECK(ncl_driver_error_tier(ncl_modbus_tcp_split(
                       kReply, sizeof(kReply), 0x9999, NULL, NULL, NULL)) == 2);
        /* a truncated frame is not an answer yet */
        NCL_CHECK_EQ_INT(ncl_modbus_tcp_split(kReply, 9, 0x1234, NULL, NULL, NULL),
                         NCL_ERR_RANGE);
    }
}

static void test_rtu_frame(void)
{
    uint8_t pdu[8];
    uint8_t frame[32];
    size_t pdu_len;
    size_t frame_len;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    size_t total = 0;

    NCL_TEST_CASE("an RTU frame carries the CRC low byte first");
    pdu_len = ncl_modbus_read_pdu(pdu, sizeof(pdu), NCL_MB_FC_READ_HOLDING, 0, 10);
    frame_len = ncl_modbus_rtu_frame(frame, sizeof(frame), 0x01, pdu, pdu_len);
    NCL_CHECK_EQ_INT(frame_len, 8);
    {
        static const uint8_t kExpected[] = {0x01, 0x03, 0x00, 0x00,
                                            0x00, 0x0A, 0xC5, 0xCD};

        NCL_CHECK(memcmp(frame, kExpected, sizeof(kExpected)) == 0);
    }
    NCL_CHECK_EQ_INT(ncl_modbus_rtu_split(frame, frame_len, 0x01, &reply,
                                          &reply_len, NULL),
                     NCL_OK);
    NCL_CHECK_EQ_INT(reply_len, 5);

    NCL_TEST_CASE("a corrupted RTU frame is a transport error, not data");
    frame[frame_len - 1] ^= 0xFF;
    NCL_CHECK(ncl_driver_error_tier(ncl_modbus_rtu_split(frame, frame_len, 1,
                                                         NULL, NULL, NULL)) == 1);
    /* another slave answering is a protocol error */
    NCL_CHECK(ncl_driver_error_tier(ncl_modbus_rtu_split(frame, frame_len, 2,
                                                         NULL, NULL, NULL)) == 2);
    frame[frame_len - 1] ^= 0xFF;

    NCL_TEST_CASE("the reply length is derived from the function code");
    {
        static const uint8_t kRead[] = {0x01, 0x03, 0x04};
        static const uint8_t kWrite[] = {0x01, 0x10};
        static const uint8_t kException[] = {0x01, 0x83};

        NCL_CHECK_EQ_INT(ncl_modbus_rtu_reply_size(kRead, sizeof(kRead), &total),
                         NCL_OK);
        NCL_CHECK_EQ_INT(total, 9);
        NCL_CHECK_EQ_INT(ncl_modbus_rtu_reply_size(kWrite, sizeof(kWrite), &total),
                         NCL_OK);
        NCL_CHECK_EQ_INT(total, 8);
        NCL_CHECK_EQ_INT(
            ncl_modbus_rtu_reply_size(kException, sizeof(kException), &total),
            NCL_OK);
        NCL_CHECK_EQ_INT(total, 5);
    }
}

static void test_areas_and_order(void)
{
    ncl_modbus_area area;
    ncl_modbus_word_order order;
    uint8_t data[8];
    ncl_json *value = NULL;
    uint8_t encoded[8];
    size_t encoded_len = 0;

    NCL_TEST_CASE("the four areas, their function codes and their limits");
    NCL_CHECK(ncl_modbus_area_parse("holding", &area) && area == NCL_MB_HOLDING);
    NCL_CHECK(ncl_modbus_area_parse("4x", &area) && area == NCL_MB_HOLDING);
    NCL_CHECK(ncl_modbus_area_parse("COIL", &area) && area == NCL_MB_COIL);
    NCL_CHECK(ncl_modbus_area_parse("discrete", &area) && area == NCL_MB_DISCRETE);
    NCL_CHECK(!ncl_modbus_area_parse("registers", &area));
    NCL_CHECK_EQ_INT(ncl_modbus_area_read_code(NCL_MB_HOLDING), 0x03);
    NCL_CHECK_EQ_INT(ncl_modbus_area_read_code(NCL_MB_DISCRETE), 0x02);
    NCL_CHECK(ncl_modbus_area_is_writable(NCL_MB_COIL));
    NCL_CHECK(!ncl_modbus_area_is_writable(NCL_MB_INPUT));
    NCL_CHECK_EQ_INT(ncl_modbus_area_max_read(NCL_MB_HOLDING), 125);
    NCL_CHECK_EQ_INT(ncl_modbus_area_max_write(NCL_MB_HOLDING), 123);
    NCL_CHECK_EQ_INT(ncl_modbus_area_max_read(NCL_MB_COIL), 2000);

    NCL_TEST_CASE("multi-register values follow the configured byte order");
    /* 1.5f = 3F C0 00 00 */
    {
        static const uint8_t kABCD[] = {0x3F, 0xC0, 0x00, 0x00};
        static const uint8_t kCDAB[] = {0x00, 0x00, 0x3F, 0xC0};
        static const uint8_t kBADC[] = {0xC0, 0x3F, 0x00, 0x00};
        static const uint8_t kDCBA[] = {0x00, 0x00, 0xC0, 0x3F};
        double real = 0;
        int i;

        for (i = 0; i < 4; i++) {
            const uint8_t *bytes = i == 0   ? kABCD
                                   : i == 1 ? kCDAB
                                   : i == 2 ? kBADC
                                            : kDCBA;

            NCL_CHECK_EQ_INT(ncl_modbus_decode(bytes, 4, 0, NCL_DTYPE_FLOAT32, 0,
                                               (ncl_modbus_word_order)i, &value),
                             NCL_OK);
            NCL_CHECK(ncl_json_as_double(value, &real));
            NCL_CHECK(real == 1.5);
            ncl_json_free(value);
            /* and the encoder is the exact inverse */
            NCL_CHECK_EQ_INT(ncl_modbus_encode(ncl_json_new_double(1.5),
                                               NCL_DTYPE_FLOAT32, 0,
                                               (ncl_modbus_word_order)i, encoded,
                                               sizeof(encoded), &encoded_len),
                             NCL_OK);
            NCL_CHECK_EQ_INT(encoded_len, 4);
            NCL_CHECK(memcmp(encoded, bytes, 4) == 0);
        }
    }

    NCL_TEST_CASE("integers keep their sign, bytes take the low one");
    memcpy(data, "\xFF\xFE", 2);
    NCL_CHECK_EQ_INT(ncl_modbus_decode(data, 2, 0, NCL_DTYPE_INT16, 0,
                                       NCL_MB_ORDER_ABCD, &value),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(value, "x", 0), 0);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, -2);
    }
    ncl_json_free(value);
    memcpy(data, "\x12\x34", 2);
    NCL_CHECK_EQ_INT(ncl_modbus_decode(data, 2, 0, NCL_DTYPE_BYTE, 0,
                                       NCL_MB_ORDER_ABCD, &value),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(value, "x", -1), -1);
    ncl_json_free(value);

    NCL_TEST_CASE("a string is the registers read as text");
    memcpy(data, "AB", 2);
    NCL_CHECK_EQ_INT(ncl_modbus_decode(data, 2, 0, NCL_DTYPE_STRING, 2,
                                       NCL_MB_ORDER_ABCD, &value),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "AB");
    ncl_json_free(value);

    NCL_TEST_CASE("byte order names round-trip");
    NCL_CHECK(ncl_modbus_word_order_parse("cdab", &order) &&
              order == NCL_MB_ORDER_CDAB);
    NCL_CHECK(!ncl_modbus_word_order_parse("XYZW", &order));
    NCL_CHECK_EQ_STR(ncl_modbus_word_order_name(NCL_MB_ORDER_DCBA), "DCBA");
}

static void test_pdu_and_exceptions(void)
{
    uint8_t pdu[64];
    size_t len;
    char message[160];

    NCL_TEST_CASE("PDU bodies match the specification");
    len = ncl_modbus_read_pdu(pdu, sizeof(pdu), NCL_MB_FC_READ_HOLDING, 0x006B, 3);
    NCL_CHECK_EQ_INT(len, 5);
    {
        static const uint8_t kExpected[] = {0x03, 0x00, 0x6B, 0x00, 0x03};

        NCL_CHECK(memcmp(pdu, kExpected, sizeof(kExpected)) == 0);
    }
    len = ncl_modbus_write_single_pdu(pdu, sizeof(pdu), NCL_MB_FC_WRITE_COIL,
                                      0x00AC, 0xFF00);
    {
        static const uint8_t kExpected[] = {0x05, 0x00, 0xAC, 0xFF, 0x00};

        NCL_CHECK(memcmp(pdu, kExpected, sizeof(kExpected)) == 0);
    }
    {
        static const uint8_t kData[] = {0x00, 0x0A, 0x01, 0x02};
        static const uint8_t kExpected[] = {0x10, 0x00, 0x01, 0x00, 0x02,
                                            0x04, 0x00, 0x0A, 0x01, 0x02};

        len = ncl_modbus_write_multi_pdu(pdu, sizeof(pdu),
                                         NCL_MB_FC_WRITE_REGISTERS, 0x0001, 2,
                                         kData, sizeof(kData));
        NCL_CHECK_EQ_INT(len, sizeof(kExpected));
        NCL_CHECK(memcmp(pdu, kExpected, sizeof(kExpected)) == 0);
    }

    NCL_TEST_CASE("exception replies become tiered, readable errors");
    {
        static const uint8_t kIllegalAddress[] = {0x83, 0x02};
        static const uint8_t kSlaveBusy[] = {0x83, 0x06};

        NCL_CHECK_EQ_INT(ncl_modbus_check_exception(kIllegalAddress, 2, 0x03,
                                                    message, sizeof(message)),
                         NCL_DRV_ERR_PROTOCOL(0x102));
        NCL_CHECK(strstr(message, "地址非法") != NULL);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_modbus_check_exception(
                             kSlaveBusy, 2, 0x03, message, sizeof(message))),
                         3); /* busy is a business condition: do not reconnect */
    }
    {
        static const uint8_t kGood[] = {0x03, 0x02, 0x00, 0x2A};
        const uint8_t *data = NULL;

        NCL_CHECK_EQ_INT(ncl_modbus_check_read_reply(kGood, sizeof(kGood), 0x03, 2,
                                                     &data, message, sizeof(message)),
                         NCL_OK);
        NCL_CHECK(data != NULL && data[1] == 0x2A);
        /* the device answering with another function code is a protocol error */
        NCL_CHECK(ncl_driver_error_tier(ncl_modbus_check_read_reply(
                       kGood, sizeof(kGood), 0x04, 2, NULL, message,
                       sizeof(message))) == 2);
        /* so is a byte count that does not match the request */
        NCL_CHECK(ncl_driver_error_tier(ncl_modbus_check_read_reply(
                       kGood, sizeof(kGood), 0x03, 4, NULL, message,
                       sizeof(message))) == 2);
    }
}

/* ============================================================ the wire ==== */

/*
 * A minimal Modbus TCP slave: 64 holding registers, 64 coils. It answers the
 * function codes the driver uses, which is enough to test the whole path -
 * request framing, span merging, decoding and error mapping - over a socket.
 */
typedef struct {
    ncl_socket *listener;
    uint16_t    holding[64];
    uint8_t     coils[8];
    unsigned    port;
    int         requests;
    int         fails_to_skip; /**< answer with a timeout this many times */
    ncl_thread *thread;
    bool        stop;
} slave;

static size_t slave_pdu(slave *s, const uint8_t *pdu, size_t len, uint8_t *out,
                        size_t cap)
{
    uint8_t function = pdu[0];
    uint16_t address = len >= 3 ? (uint16_t)((pdu[1] << 8) | pdu[2]) : 0;
    uint16_t count = len >= 5 ? (uint16_t)((pdu[3] << 8) | pdu[4]) : 0;

    s->requests++;
    switch (function) {
    case NCL_MB_FC_READ_HOLDING: {
        size_t bytes = (size_t)count * 2u;

        if (address + count > 64 || bytes + 2 > cap) {
            out[0] = 0x83;
            out[1] = 0x02;
            return 2;
        }
        out[0] = function;
        out[1] = (uint8_t)bytes;
        {
            size_t i;

            for (i = 0; i < count; i++) {
                out[2 + i * 2] = (uint8_t)(s->holding[address + i] >> 8);
                out[3 + i * 2] = (uint8_t)s->holding[address + i];
            }
        }
        return bytes + 2;
    }
    case NCL_MB_FC_WRITE_REGISTER: {
        uint16_t value = len >= 5 ? (uint16_t)((pdu[3] << 8) | pdu[4]) : 0;

        if (address >= 64) {
            out[0] = 0x86;
            out[1] = 0x02;
            return 2;
        }
        s->holding[address] = value;
        memcpy(out, pdu, 5);
        return 5;
    }
    case NCL_MB_FC_WRITE_REGISTERS: {
        size_t i;

        if (address + count > 64 || len < 6) {
            out[0] = 0x90;
            out[1] = 0x03;
            return 2;
        }
        for (i = 0; i < count; i++) {
            s->holding[address + i] =
                (uint16_t)((pdu[6 + i * 2] << 8) | pdu[7 + i * 2]);
        }
        out[0] = function;
        out[1] = pdu[1];
        out[2] = pdu[2];
        out[3] = pdu[3];
        out[4] = pdu[4];
        return 5;
    }
    case NCL_MB_FC_READ_COILS: {
        size_t bytes = ((size_t)count + 7u) / 8u;
        size_t i;

        if (address + count > 64 || bytes + 2 > cap) {
            out[0] = 0x81;
            out[1] = 0x02;
            return 2;
        }
        out[0] = function;
        out[1] = (uint8_t)bytes;
        memset(out + 2, 0, bytes);
        for (i = 0; i < count; i++) {
            uint16_t coil = (uint16_t)(address + i);
            uint8_t bit = (uint8_t)((s->coils[coil / 8u] >> (coil % 8u)) & 1u);

            out[2 + i / 8u] |= (uint8_t)(bit << (i % 8u));
        }
        return bytes + 2;
    }
    case NCL_MB_FC_WRITE_COIL: {
        uint16_t word = (uint16_t)((pdu[3] << 8) | pdu[4]);

        if (address >= 64) {
            out[0] = 0x85;
            out[1] = 0x02;
            return 2;
        }
        if (word == 0xFF00) {
            s->coils[address / 8u] |= (uint8_t)(1u << (address % 8u));
        } else {
            s->coils[address / 8u] &= (uint8_t)~(1u << (address % 8u));
        }
        memcpy(out, pdu, 5);
        return 5;
    }
    case 0x08:
        memcpy(out, pdu, len);
        return len;
    default:
        out[0] = (uint8_t)(function | 0x80);
        out[1] = 0x01;
        return 2;
    }
}

static void slave_main(void *arg)
{
    slave *s = (slave *)arg;

    while (!s->stop) {
        ncl_socket *peer = ncl_socket_accept(s->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t header[7];
            uint8_t pdu[260];
            uint8_t reply[260];
            uint8_t frame[280];
            size_t length;
            size_t reply_len;
            size_t frame_len;
            uint16_t transaction;

            if (ncl_socket_recv_exact(peer, header, 7, 2000) != NCL_OK) {
                break;
            }
            length = ((size_t)header[4] << 8) | header[5];
            if (length < 2 || length > sizeof(pdu) + 1) {
                break;
            }
            if (ncl_socket_recv_exact(peer, pdu, length - 1, 2000) != NCL_OK) {
                break;
            }
            if (s->fails_to_skip > 0) { /* a device that says nothing */
                s->fails_to_skip--;
                ncl_sleep_millis(200);
                break;
            }
            reply_len = slave_pdu(s, pdu, length - 1, reply, sizeof(reply));
            transaction = (uint16_t)((header[0] << 8) | header[1]);
            frame_len = ncl_modbus_tcp_frame(frame, sizeof(frame), transaction,
                                             header[6], reply, reply_len);
            if (ncl_socket_send(peer, frame, frame_len) != NCL_OK) {
                break;
            }
        }
        ncl_socket_close(peer);
    }
}

static slave *slave_start(void)
{
    slave *s = (slave *)ncl_mem_calloc(1, sizeof(*s));
    size_t i;

    if (s == NULL) {
        return NULL;
    }
    s->listener = ncl_socket_listen(0, NULL, 0);
    if (s->listener == NULL) {
        ncl_free_safe(s);
        return NULL;
    }
    s->port = ncl_socket_local_port(s->listener);
    for (i = 0; i < 64; i++) {
        s->holding[i] = (uint16_t)(1000 + i);
    }
    s->thread = ncl_thread_start(slave_main, s);
    if (s->thread == NULL) {
        ncl_socket_close(s->listener);
        ncl_free_safe(s);
        return NULL;
    }
    return s;
}

static void slave_stop(slave *s)
{
    if (s == NULL) {
        return;
    }
    /* The accept loop polls, so it notices on its own; closing the listener
     * first would pull the socket out from under the thread. */
    s->stop = true;
    ncl_thread_join(s->thread);
    ncl_socket_close(s->listener);
    ncl_free_safe(s);
}

static ncl_driver *tcp_driver(slave *s, int retries)
{
    ncl_driver *driver = ncl_modbus_tcp_create();
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,\"unit\":1,"
                            "\"timeoutMs\":600,\"retries\":%d}",
                            s->port, retries);
    params = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (params == NULL || driver->ops->create(driver, params) != NCL_OK) {
        ncl_json_free(params);
        driver->ops->destroy(driver);
        return NULL;
    }
    ncl_json_free(params);
    return driver;
}

static ncl_err parse_address(const char *spec, ncl_address *address)
{
    ncl_json *node = ncl_json_parse_cstr(spec, NULL);
    ncl_err err;

    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, address);
    ncl_json_free(node);
    return err;
}

/** Read one address through read_one() and answer its integer value. */
static long long read_int(ncl_driver *driver, const char *spec, ncl_err *err)
{
    ncl_address address;
    ncl_json *value = NULL;
    long long number = -1;

    *err = parse_address(spec, &address);
    if (*err != NCL_OK) {
        return -1;
    }
    *err = ncl_driver_read_one(driver, &address, &value);
    ncl_address_clear(&address);
    if (*err == NCL_OK && !ncl_json_as_int(value, &number)) {
        double real = 0;

        if (ncl_json_as_double(value, &real)) {
            number = (long long)real;
        } else {
            *err = NCL_ERR_INVALID_VALUE;
        }
    }
    ncl_json_free(value);
    return number;
}

static void test_tcp_against_a_slave(void)
{
    slave *s = slave_start();
    ncl_driver *driver;
    ncl_err err = NCL_ERR;
    long long number;

    NCL_TEST_CASE("a real socket: reads and writes reach the device");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = tcp_driver(s, 1);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        slave_stop(s);
        return;
    }
    number = read_int(driver, "{\"area\":\"holding\",\"offset\":7}", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 1007);
    NCL_CHECK(s->requests >= 1);

    NCL_TEST_CASE("a write is echoed and the device keeps the value");
    {
        ncl_json *node = ncl_json_parse_cstr(
            "{\"area\":\"holding\",\"offset\":10}", NULL);
        ncl_address address;

        NCL_CHECK_EQ_INT(ncl_address_from_json(node, &address), NCL_OK);
        ncl_json_free(node);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address,
                                              ncl_json_new_int(4242)),
                         NCL_OK);
        ncl_address_clear(&address);
    }
    number = read_int(driver, "{\"area\":\"holding\",\"offset\":10}", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 4242);

    NCL_TEST_CASE("a batch over a gap is merged into one request");
    {
        ncl_address addresses[2];
        ncl_json *values = NULL;
        int before = s->requests;

        /* 0..3 and 6..7: a gap of 2 registers, inside the merge threshold */
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"holding\",\"offset\":0,"
                                       "\"length\":4}",
                                       &addresses[0]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"holding\",\"offset\":6,"
                                       "\"length\":2}",
                                       &addresses[1]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 2, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->requests - before, 1);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_arr_get(values, 0)), 4);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_arr_get(values, 1)), 2);
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);
    }

    NCL_TEST_CASE("a gap beyond the threshold is two requests");
    {
        ncl_address addresses[2];
        ncl_json *values = NULL;
        int before = s->requests;

        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"holding\",\"offset\":0}",
                                       &addresses[0]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"holding\",\"offset\":40}",
                                       &addresses[1]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 2, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->requests - before, 2);
        {
            long long value = 0;

            NCL_CHECK(ncl_json_as_int(ncl_json_arr_get(values, 1), &value));
            NCL_CHECK_EQ_INT(value, 1040);
        }
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);
    }

    NCL_TEST_CASE("float and bit points decode end to end");
    {
        ncl_address address;
        ncl_json *value = NULL;
        double real = 0;

        /* 1.5 as a float in registers 20..21 (ABCD: 3FC0 0000) */
        s->holding[20] = 0x3FC0;
        s->holding[21] = 0x0000;
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"holding\",\"offset\":20,"
                                       "\"dtype\":\"float32\"}",
                                       &address),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_read_one(driver, &address, &value), NCL_OK);
        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
        ncl_json_free(value);
        ncl_address_clear(&address);

        s->coils[1] = 0x04; /* coil 10 = byte 1, bit 2 */
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"coil\",\"offset\":10,"
                                       "\"dtype\":\"bit\"}",
                                       &address),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_read_one(driver, &address, &value), NCL_OK);
        {
            bool set = false;

            NCL_CHECK(ncl_json_as_bool(value, &set) && set);
        }
        ncl_json_free(value);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("writing a discrete input is refused, not attempted");
    {
        ncl_address address;

        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"discrete\",\"offset\":1}",
                                       &address),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address,
                                              ncl_json_new_bool(true)),
                         NCL_DRV_ERR_BUSINESS(0x22));
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("an illegal address is the device's protocol error");
    {
        /* beyond the slave's 64 registers: the device answers 0x02 */
        number = read_int(driver, "{\"area\":\"holding\",\"offset\":900}", &err);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 2);
        NCL_CHECK_EQ_INT(number, -1);
    }

    NCL_TEST_CASE("the loopback method proves the wire is alive");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "loopback", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "echo", -1), 0xA537);
        ncl_json_free(result);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("the raw hatch sends a PDU and returns the reply");
    {
        ncl_driver_result out;
        const uint8_t pdu[] = {0x03, 0x00, 0x00, 0x00, 0x01};

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, pdu, sizeof(pdu), &out),
                         NCL_OK);
        NCL_CHECK(out.success);
        NCL_CHECK_EQ_INT(out.raw_len, 4);
        /* 1000 = 0x03E8, so the payload is 03 02 03 E8 */
        NCL_CHECK_EQ_STR(ncl_json_as_string(out.value), "030203e8");
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("the session closes and reopens on demand");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    number = read_int(driver, "{\"area\":\"holding\",\"offset\":1}", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 1001);
    NCL_CHECK(driver->ops->is_connected(driver));

    driver->ops->destroy(driver);
    slave_stop(s);
}

static void test_silent_device(void)
{
    slave *s = slave_start();
    ncl_driver *driver;
    ncl_err err = NCL_ERR;

    NCL_TEST_CASE("a device that stops answering is a transport failure");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = tcp_driver(s, 0);
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        s->fails_to_skip = 1; /* the next request gets no answer at all */
        (void)read_int(driver, "{\"area\":\"holding\",\"offset\":0}", &err);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);
        driver->ops->destroy(driver);
    }
    slave_stop(s);
}

static void test_rtu_needs_a_port(void)
{
    ncl_driver *driver = ncl_modbus_rtu_create();
    ncl_json *params;

    NCL_TEST_CASE("RTU without a serial port is a configuration error");
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        return;
    }
    params = ncl_json_parse_cstr("{\"host\":\"127.0.0.1\"}", NULL);
    NCL_CHECK_EQ_INT(driver->ops->create(driver, params), NCL_ERR_INVALID_ARG);
    ncl_json_free(params);
    /* with one, it accepts the settings and only fails when it cannot open */
    params = ncl_json_parse_cstr(
        "{\"serial\":\"COM-not-there\",\"baud\":19200,\"parity\":\"E\","
        "\"unit\":7,\"retries\":2}",
        NULL);
    NCL_CHECK_EQ_INT(driver->ops->create(driver, params), NCL_OK);
    ncl_json_free(params);
    NCL_CHECK_EQ_STR(ncl_driver_protocol(driver), "modbus_rtu");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(driver->ops->open(driver)), 1);
    driver->ops->destroy(driver);
}

/** The whole chain a deployment uses: configuration -> manager -> device. */
static void test_through_the_manager(void)
{
    slave *s = slave_start();
    test_point_map *manager = test_point_map_create(ncl_modbus_tcp_create);
    ncl_strbuf err;
    ncl_strbuf json;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured link reads through the point map");
    NCL_CHECK(s != NULL && manager != NULL);
    if (s == NULL || manager == NULL) {
        slave_stop(s);
        test_point_map_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"plc\",\"path\":\"/PLC\","
                            "\"type\":\"modbus_tcp\","
                            "\"parameters\":{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":600},"
                            "\"points\":["
                            "{\"path\":\"/PLC/TEMP\",\"addr\":\"4x12\"},"
                            "{\"path\":\"/PLC/SPEED\",\"addr\":"
                            "{\"area\":\"holding\",\"offset\":30,"
                            "\"dtype\":\"float32\"},\"writable\":true}]}",
                            s->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    ncl_strbuf_init(&err);
    if (config != NULL) {
        NCL_CHECK_EQ_INT(test_point_map_add_json(manager, config, &err),
                         NCL_OK);
    }
    if (err.len > 0) {
        printf("    %s\n", ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    ncl_json_free(config);
    /* Both points of the one link landed in the map. */
    NCL_CHECK_EQ_INT(test_point_map_count(manager), 2);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/TEMP", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 1012);
    ncl_json_free(value);
    value = NULL;

    /* the float point, written as a register pair */
    NCL_CHECK_EQ_INT(test_point_map_write(manager, "/PLC/SPEED",
                                              ncl_json_new_double(2.5)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(s->holding[30], 0x4020);
    NCL_CHECK_EQ_INT(s->holding[31], 0x0000);

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    slave_stop(s);
}

NCL_TEST_MAIN_BEGIN()
    test_crc();
    test_tcp_frame();
    test_rtu_frame();
    test_areas_and_order();
    test_pdu_and_exceptions();
    test_tcp_against_a_slave();
    test_silent_device();
    test_rtu_needs_a_port();
    test_through_the_manager();
NCL_TEST_MAIN_END()
