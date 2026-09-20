/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Omron FINS: the byte level against 12-OMRON-FINS.md §6, then a real socket
 * against a mock PLC the test runs itself (handshake included).
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_driver_manager.h"
#include "nclink/clients/fins.h"
#include "fins/ncl_fins_driver.h"

static bool binary_equal(const uint8_t *actual, const char *expected, size_t len)
{
    return memcmp(actual, expected, len) == 0;
}

/* ============================================================ the bytes == */

static void test_areas(void)
{
    uint8_t code = 0;

    NCL_TEST_CASE("area codes, including the EM banks");
    NCL_CHECK(ncl_fins_area_lookup("D", &code) && code == 0x82);
    NCL_CHECK(ncl_fins_area_lookup("CIO", &code) && code == 0xB0);
    NCL_CHECK(ncl_fins_area_lookup("w", &code) && code == 0xB1);
    NCL_CHECK(ncl_fins_area_lookup("H", &code) && code == 0xB2);
    NCL_CHECK(ncl_fins_area_lookup("A", &code) && code == 0xB3);
    NCL_CHECK(ncl_fins_area_lookup("E0_0", &code) && code == 0x20);
    NCL_CHECK(ncl_fins_area_lookup("E0_15", &code) && code == 0x2F);
    NCL_CHECK(ncl_fins_area_lookup("E1_15", &code) && code == 0x5F);
    NCL_CHECK(!ncl_fins_area_lookup("E2_0", &code)); /* no such bank */
    NCL_CHECK(!ncl_fins_area_lookup("E0_16", &code)); /* no such library */
    NCL_CHECK(!ncl_fins_area_lookup("Q", &code));
    NCL_CHECK_EQ_STR(ncl_fins_area_name(0x82), "D");
    NCL_CHECK(ncl_fins_area_is_bit(0xB0));
    NCL_CHECK(!ncl_fins_area_is_bit(0x82));
}

static void test_frames(void)
{
    uint8_t frame[64];
    uint8_t body[8];
    ncl_fins_header header;
    size_t len;
    size_t frame_len = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    uint32_t tcp_command = 0;
    uint16_t command = 0;

    NCL_TEST_CASE("the node address allocation request of §2");
    len = ncl_fins_node_request(frame, sizeof(frame), 1);
    NCL_CHECK_EQ_INT(len, 20);
    NCL_CHECK(binary_equal(frame, "\x46\x49\x4E\x53\x00\x00\x00\x0C"
                                  "\x00\x00\x00\x00\x00\x00\x00\x00"
                                  "\x00\x00\x00\x01",
                           20));

    NCL_TEST_CASE("a memory read of D100 for 10 words");
    ncl_fins_header_default(&header);
    header.da1 = 10; /* the PLC's node */
    header.sa1 = 1;  /* ours */
    header.sid = 1;
    NCL_CHECK_EQ_INT(ncl_fins_read_body(body, sizeof(body), 0x82, 100, 0x00, 10),
                     6);
    NCL_CHECK(binary_equal(body, "\x82\x00\x64\x00\x00\x0A", 6));
    {
        uint8_t fins[64];
        size_t fins_len = ncl_fins_frame(fins, sizeof(fins), &header,
                                         NCL_FINS_CMD_MEMORY_READ, body, 6);

        NCL_CHECK_EQ_INT(fins_len, 18);
        NCL_CHECK(binary_equal(fins, "\x80\x00\x02\x00\x0A\x00\x00\x01\x00\x01"
                                     "\x01\x01\x82\x00\x64\x00\x00\x0A",
                               18));
        len = ncl_fins_tcp_frame(frame, sizeof(frame), NCL_FINS_TCP_DATA_SEND, fins,
                                 fins_len);
        NCL_CHECK_EQ_INT(len, 34); /* 16 + 18 */
        NCL_CHECK(binary_equal(frame, "\x46\x49\x4E\x53\x00\x00\x00\x1A"
                                      "\x00\x00\x00\x01\x00\x00\x00\x00",
                               16));
    }

    NCL_TEST_CASE("a transport frame splits back into the FINS frame");
    {
        static const uint8_t kReply[] = {
            0x46, 0x49, 0x4E, 0x53, 0x00, 0x00, 0x00, 0x17, /* length 23 */
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
            0x80, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x0A, /* the PLC is now */
            0x00, 0x01,                                     /* the source     */
            0x01, 0x01,                                     /* read command   */
            0x00, 0x00, 0x00,                               /* end code       */
        };
        const uint8_t *fins = NULL;
        size_t fins_len = 0;
        const uint8_t *reply = NULL;
        size_t reply_len = 0;

        NCL_CHECK_EQ_INT(ncl_fins_tcp_split(kReply, sizeof(kReply), &tcp_command,
                                            NULL, &data, &data_len, &frame_len),
                         NCL_OK);
        NCL_CHECK_EQ_INT(tcp_command, NCL_FINS_TCP_DATA_SEND);
        NCL_CHECK_EQ_INT(frame_len, sizeof(kReply));
        NCL_CHECK_EQ_INT(data_len, 15);
        fins = data;
        fins_len = data_len;
        NCL_CHECK_EQ_INT(ncl_fins_split(fins, fins_len, &header, &command, &reply,
                                        &reply_len, NULL),
                         NCL_OK);
        NCL_CHECK_EQ_INT(command, NCL_FINS_CMD_MEMORY_READ);
        NCL_CHECK_EQ_INT(header.sa1, 0x0A); /* the two ends swapped roles */
        NCL_CHECK_EQ_INT(reply_len, 3);
        NCL_CHECK_EQ_INT(reply[0], 0x00); /* end code OK */
        /* a frame that is still arriving is not an error */
        NCL_CHECK_EQ_INT(ncl_fins_tcp_split(kReply, 10, NULL, NULL, NULL, NULL,
                                            NULL),
                         NCL_ERR_RANGE);
        NCL_CHECK(ncl_driver_error_tier(ncl_fins_tcp_split(
                       kReply + 1, sizeof(kReply) - 1, NULL, NULL, NULL, NULL,
                       NULL)) == 2); /* lost the magic */
    }

    NCL_TEST_CASE("the handshake reply carries the two node numbers");
    {
        static const uint8_t kPayload[] = {0x00, 0x00, 0x00, 0x07,
                                           0x00, 0x00, 0x00, 0x0A};
        uint8_t client = 0;
        uint8_t server = 0;

        NCL_CHECK_EQ_INT(ncl_fins_node_response(kPayload, sizeof(kPayload),
                                                &client, &server),
                         NCL_OK);
        NCL_CHECK_EQ_INT(client, 7);
        NCL_CHECK_EQ_INT(server, 10);
        NCL_CHECK_EQ_INT(ncl_fins_node_response(kPayload, 4, &client, &server),
                         NCL_ERR_RANGE);
    }
}

static void test_replies(void)
{
    char message[160];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    NCL_TEST_CASE("the reply header is the end code and its two detail bytes");
    {
        static const uint8_t kData[] = {0x00, 0x00, 0x00, 0x03, 0xE8};

        NCL_CHECK_EQ_INT(ncl_fins_reply_begin(kData, sizeof(kData), &payload,
                                              &payload_len, message,
                                              sizeof(message)),
                         NCL_OK);
        NCL_CHECK_EQ_INT(payload_len, 2);
        NCL_CHECK_EQ_INT(payload[0], 0x03); /* the word, big endian */
        NCL_CHECK_EQ_INT(ncl_fins_reply_begin(kData, 2, &payload, &payload_len,
                                              message, sizeof(message)),
                         NCL_ERR_RANGE);
    }

    NCL_TEST_CASE("end codes become tiered, readable errors");
    NCL_CHECK_EQ_INT(ncl_fins_check_end_code(0x00, 0, message, sizeof(message)),
                     NCL_OK);
    /* a lost token or a timeout is a link problem: reconnect and re-send */
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_fins_check_end_code(
                         0x04, 0, message, sizeof(message))),
                     1);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_fins_check_end_code(
                         0x03, 0, message, sizeof(message))),
                     1);
    /* "cannot read there" is definitive: the frame was fine */
    NCL_CHECK_EQ_INT(ncl_fins_check_end_code(0x20, 0, message, sizeof(message)),
                     NCL_DRV_ERR_PROTOCOL(0x220));
    NCL_CHECK(strstr(message, "读取不可能") != NULL);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_fins_check_end_code(
                         0x21, 0, message, sizeof(message))),
                     2);
    /* a busy or refused controller is the device's business */
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_fins_check_end_code(
                         0xA5, 0, message, sizeof(message))),
                     3);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_fins_check_end_code(
                         0x99, 0, message, sizeof(message))),
                     2);
}

static void test_data(void)
{
    ncl_json *value = NULL;
    uint8_t bytes[8];

    NCL_TEST_CASE("words are big endian");
    memcpy(bytes, "\x03\xE8", 2); /* 1000 */
    NCL_CHECK_EQ_INT(ncl_fins_decode(bytes, 2, 0, NCL_DTYPE_INT16, 0, &value),
                     NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 1000);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("1.5f is 3F C0 00 00 on the wire");
    memcpy(bytes, "\x3F\xC0\x00\x00", 4);
    NCL_CHECK_EQ_INT(ncl_fins_decode(bytes, 4, 0, NCL_DTYPE_FLOAT32, 0, &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
    }
    ncl_json_free(value);
    {
        size_t len = 0;

        NCL_CHECK_EQ_INT(ncl_fins_encode(ncl_json_new_double(1.5),
                                         NCL_DTYPE_FLOAT32, 0, bytes,
                                         sizeof(bytes), &len),
                         NCL_OK);
        NCL_CHECK_EQ_INT(len, 4);
        NCL_CHECK(binary_equal(bytes, "\x3F\xC0\x00\x00", 4));
    }

    NCL_TEST_CASE("a bit is one byte, a string is the words read as text");
    bytes[0] = 1;
    NCL_CHECK_EQ_INT(ncl_fins_decode(bytes, 1, 0, NCL_DTYPE_BIT, 0, &value),
                     NCL_OK);
    {
        bool set = false;

        NCL_CHECK(ncl_json_as_bool(value, &set) && set);
    }
    ncl_json_free(value);
    memcpy(bytes, "AB", 2);
    NCL_CHECK_EQ_INT(ncl_fins_decode(bytes, 2, 0, NCL_DTYPE_STRING, 2, &value),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "AB");
    ncl_json_free(value);
}

/* ============================================================= the wire == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    int         requests;
    int         fails_to_skip;
    int         handshakes;
    uint8_t     client_node;
    uint16_t    words[512]; /**< D */
    uint8_t     bits[32];   /**< CIO */
} fins_slave;

static void fins_slave_write_header(uint8_t *out, uint32_t command_lo,
                                    uint32_t length)
{
    memcpy(out, "FINS", 4);
    out[4] = (uint8_t)(length >> 24);
    out[5] = (uint8_t)(length >> 16);
    out[6] = (uint8_t)(length >> 8);
    out[7] = (uint8_t)length;
    out[8] = 0;
    out[9] = 0;
    out[10] = 0;
    out[11] = (uint8_t)command_lo; /* 0 = node allocation, 1 = data send */
    out[12] = 0; /* error code */
    out[13] = 0;
    out[14] = 0;
    out[15] = 0;
}

/** Answer a request FINS frame; returns the reply FINS frame length. */
static size_t fins_slave_reply(fins_slave *s, const uint8_t *fins, size_t fins_len,
                               uint8_t *out, size_t cap)
{
    uint16_t command = (uint16_t)((fins[10] << 8) | fins[11]);
    const uint8_t *body = fins + 12;
    size_t body_len = fins_len - 12;
    uint8_t end_code = 0;
    size_t used = 0;

    (void)cap;
    s->requests++;
    /* The reply header: the same ten bytes with the two ends swapped, as a PLC
     * answers. */
    out[0] = 0x80;
    out[1] = fins[1];
    out[2] = fins[2];
    out[3] = fins[6];
    out[4] = fins[7];
    out[5] = fins[8];
    out[6] = fins[3];
    out[7] = fins[4];
    out[8] = fins[5];
    out[9] = fins[9]; /* the service id comes back */
    out[10] = fins[10];
    out[11] = fins[11];
    used = 12;
    if (command == NCL_FINS_CMD_MEMORY_READ) {
        uint8_t area = body[0];
        uint16_t address = (uint16_t)((body[1] << 8) | body[2]);
        uint8_t bit = body[3];
        uint16_t count = (uint16_t)((body[4] << 8) | body[5]);
        uint16_t i;

        if (body_len < 6) {
            end_code = 0x10; /* command format error */
            goto finish;
        }
        if (area == 0x82) {
            if (address + count > 512) {
                end_code = 0x20;
            } else {
                for (i = 0; i < count; i++) {
                    out[used++] = (uint8_t)(s->words[address + i] >> 8);
                    out[used++] = (uint8_t)s->words[address + i];
                }
            }
        } else if (area == 0xB0) {
            uint32_t word = address;
            uint8_t which = bit;
            uint16_t i;

            if (word >= 512 || which > 15) {
                end_code = 0x20;
            } else {
                for (i = 0; i < count; i++) {
                    uint32_t at = word + i;

                    out[used++] = (uint8_t)((s->bits[at / 8u] >> (at % 8u)) & 1u);
                }
                (void)which;
            }
        } else {
            end_code = 0x20;
        }
    } else if (command == NCL_FINS_CMD_MEMORY_WRITE) {
        uint8_t area = body[0];
        uint16_t address = (uint16_t)((body[1] << 8) | body[2]);
        uint16_t count = (uint16_t)((body[4] << 8) | body[5]);
        uint16_t i;

        if (body_len < 6) {
            end_code = 0x10;
            goto finish;
        }
        if (area == 0x82 && address + count <= 512 &&
            body_len >= 6u + (size_t)count * 2u) {
            for (i = 0; i < count; i++) {
                s->words[address + i] = (uint16_t)((body[6 + i * 2] << 8) |
                                                   body[7 + i * 2]);
            }
        } else if (area == 0xB0 && address + count <= 512 &&
                   body_len >= 6u + count) {
            for (i = 0; i < count; i++) {
                uint32_t at = address + i;

                if (body[6 + i] != 0) {
                    s->bits[at / 8u] |= (uint8_t)(1u << (at % 8u));
                } else {
                    s->bits[at / 8u] &= (uint8_t)~(1u << (at % 8u));
                }
            }
        } else {
            end_code = 0x21;
        }
    } else if (command == NCL_FINS_CMD_RUN || command == NCL_FINS_CMD_STOP) {
        /* accepted */
    } else if (command == NCL_FINS_CMD_CONTROLLER_STATUS) {
        out[used++] = 0x00; /* the controller's status words */
        out[used++] = 0x02;
    } else {
        end_code = 0x10;
    }
finish:
    /* The end code and its two detail bytes come before the data. */
    if (used > 12) {
        memmove(out + 15, out + 12, used - 12);
    }
    out[12] = end_code;
    out[13] = 0;
    out[14] = 0;
    return used + 3;
}

static void fins_slave_main(void *arg)
{
    fins_slave *s = (fins_slave *)arg;

    while (!s->stop) {
        ncl_socket *peer = ncl_socket_accept(s->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t header[16];
            uint8_t payload[2048];
            uint8_t reply[2048];
            uint8_t frame[2100];
            size_t length;
            size_t reply_len;
            size_t frame_len;

            if (ncl_socket_recv_exact(peer, header, 16, 2000) != NCL_OK) {
                break;
            }
            length = ((size_t)header[4] << 24) | ((size_t)header[5] << 16) |
                     ((size_t)header[6] << 8) | header[7];
            if (length < 8 || length > sizeof(payload) + 8) {
                break;
            }
            if (ncl_socket_recv_exact(peer, payload, length - 8u, 2000) != NCL_OK) {
                break;
            }
            if (header[8] == 0 && header[9] == 0 && header[10] == 0 &&
                header[11] == 0) { /* the node address allocation of §2 */
                s->handshakes++;
                s->client_node = length >= 12 ? payload[3] : 1;
                if (s->fails_to_skip > 0) {
                    s->fails_to_skip--;
                    break;
                }
                fins_slave_write_header(frame, 0, 16); /* 8 + 8 node numbers */
                frame[16] = 0; /* the node this client got   */
                frame[17] = 0;
                frame[18] = 0;
                frame[19] = s->client_node;
                frame[20] = 0; /* the node of this PLC       */
                frame[21] = 0;
                frame[22] = 0;
                frame[23] = 10;
                if (ncl_socket_send(peer, frame, 24) != NCL_OK) {
                    break;
                }
                continue;
            }
            if (s->fails_to_skip > 0) {
                s->fails_to_skip--;
                ncl_sleep_millis(200);
                break;
            }
            reply_len = fins_slave_reply(s, payload, length - 8u, reply,
                                         sizeof(reply));
            fins_slave_write_header(frame, 1, (uint32_t)(8u + reply_len));
            memcpy(frame + 16, reply, reply_len);
            frame_len = 16 + reply_len;
            if (ncl_socket_send(peer, frame, frame_len) != NCL_OK) {
                break;
            }
        }
        ncl_socket_close(peer);
    }
}

static fins_slave *fins_slave_start(void)
{
    fins_slave *s = (fins_slave *)ncl_mem_calloc(1, sizeof(*s));
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
    for (i = 0; i < 512; i++) {
        s->words[i] = (uint16_t)(900 + i);
    }
    s->thread = ncl_thread_start(fins_slave_main, s);
    if (s->thread == NULL) {
        ncl_socket_close(s->listener);
        ncl_free_safe(s);
        return NULL;
    }
    return s;
}

static void fins_slave_stop(fins_slave *s)
{
    if (s == NULL) {
        return;
    }
    s->stop = true;
    ncl_thread_join(s->thread);
    ncl_socket_close(s->listener);
    ncl_free_safe(s);
}

static ncl_driver *fins_driver(fins_slave *s, const char *extra)
{
    ncl_driver *driver = ncl_driver_create("fins_tcp");
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":600,\"retries\":0%s}",
                            s->port, extra != NULL ? extra : "");
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
        *err = NCL_ERR_INVALID_VALUE;
    }
    ncl_json_free(value);
    return number;
}

static void test_against_a_plc(void)
{
    fins_slave *s = fins_slave_start();
    ncl_driver *driver;
    ncl_err err = NCL_ERR;
    long long number;

    NCL_TEST_CASE("the first frame is the node address allocation");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = fins_driver(s, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        fins_slave_stop(s);
        return;
    }
    number = read_int(driver, "\"D10\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 910);
    NCL_CHECK_EQ_INT(s->handshakes, 1);

    NCL_TEST_CASE("writing a word and reading it back");
    {
        ncl_address address;

        NCL_CHECK_EQ_INT(parse_address("\"D20\"", &address), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address,
                                              ncl_json_new_int(4711)),
                         NCL_OK);
        ncl_address_clear(&address);
        NCL_CHECK_EQ_INT(s->words[20], 4711);
    }
    number = read_int(driver, "\"D20\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 4711);

    NCL_TEST_CASE("a bit of CIO is read in bit access");
    {
        ncl_address address;
        ncl_json *value = NULL;
        bool set = false;

        s->bits[1] = 0x10; /* CIO 12 on */
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"CIO\",\"offset\":12,"
                                       "\"dtype\":\"bit\"}",
                                       &address),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_read_one(driver, &address, &value), NCL_OK);
        NCL_CHECK(ncl_json_as_bool(value, &set) && set);
        ncl_json_free(value);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("a batch over a gap is merged, a distant one is not");
    {
        ncl_address addresses[2];
        ncl_json *values = NULL;
        int before = s->requests;

        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"D\",\"offset\":0,"
                                       "\"length\":4}",
                                       &addresses[0]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"D\",\"offset\":6,"
                                       "\"length\":2}",
                                       &addresses[1]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 2, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->requests - before, 1);
        {
            long long last = 0;

            NCL_CHECK(ncl_json_as_int(
                ncl_json_arr_get(ncl_json_arr_get(values, 1), 1), &last));
            NCL_CHECK_EQ_INT(last, 907);
        }
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);

        before = s->requests;
        NCL_CHECK_EQ_INT(parse_address("\"D0\"", &addresses[0]), NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("\"D400\"", &addresses[1]), NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 2, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->requests - before, 2);
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);
    }

    NCL_TEST_CASE("the controller's own end code becomes a protocol error");
    number = read_int(driver, "\"D900\"", &err);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 2);
    NCL_CHECK_EQ_INT(number, -1);

    NCL_TEST_CASE("the controller methods reach the PLC");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "controllerStatus", NULL,
                                           &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "bytes", -1), 2);
        ncl_json_free(result);
        NCL_CHECK_EQ_INT(driver->ops->call(driver, "run", NULL, NULL), NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->call(driver, "stop", NULL, NULL), NCL_OK);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("the raw hatch sends MRC/SRC and returns the reply body");
    {
        ncl_driver_result out;
        /* 06 01: read controller status, no body */
        const uint8_t raw[] = {0x06, 0x01};

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, raw, sizeof(raw), &out),
                         NCL_OK);
        /* the answer is the reply body: the two status bytes our PLC sent */
        NCL_CHECK_EQ_STR(ncl_json_as_string(out.value), "0002");
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("the session reopens on demand, handshake included");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    number = read_int(driver, "\"D1\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 901);
    NCL_CHECK_EQ_INT(s->handshakes, 2);

    driver->ops->destroy(driver);
    fins_slave_stop(s);
}

static void test_silence(void)
{
    fins_slave *s = fins_slave_start();
    ncl_driver *driver;
    ncl_err err = NCL_ERR;

    NCL_TEST_CASE("a device that stops answering is a transport failure");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = fins_driver(s, NULL);
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        s->fails_to_skip = 1;
        (void)read_int(driver, "\"D0\"", &err);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);
        driver->ops->destroy(driver);
    }
    fins_slave_stop(s);
}

static void test_through_the_manager(void)
{
    fins_slave *s = fins_slave_start();
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured FINS link reads through the point map");
    NCL_CHECK(s != NULL && manager != NULL);
    if (s == NULL || manager == NULL) {
        fins_slave_stop(s);
        ncl_driver_manager_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"plc\",\"path\":\"/PLC\","
                            "\"type\":\"fins_tcp\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":600},"
                            "\"points\":["
                            "{\"path\":\"/PLC/TEMP\",\"addr\":\"D30\"},"
                            "{\"path\":\"/PLC/SPEED\",\"addr\":"
                            "{\"area\":\"D\",\"offset\":40,"
                            "\"dtype\":\"int32\"},\"writable\":true}]}",
                            s->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    ncl_strbuf_init(&err);
    if (config != NULL) {
        NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, config, &err),
                         NCL_OK);
    }
    if (err.len > 0) {
        printf("    %s\n", ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    ncl_json_free(config);

    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/PLC/TEMP", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 930);
    ncl_json_free(value);
    value = NULL;

    s->words[40] = 0x0001; /* 65537, big endian across the two words */
    s->words[41] = 0x0001;
    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/PLC/SPEED", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 65537);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/PLC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    ncl_driver_manager_free(manager);
    fins_slave_stop(s);
}

NCL_TEST_MAIN_BEGIN()
    test_areas();
    test_frames();
    test_replies();
    test_data();
    test_against_a_plc();
    test_silence();
    test_through_the_manager();
NCL_TEST_MAIN_END()
