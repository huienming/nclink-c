/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Mitsubishi MC / SLMP: the byte level against the frames of the spec book,
 * then a real socket against a mock PLC the test runs itself.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "test_point_map.h"
#include "nclink/clients/mc.h"
#include "mc/ncl_mc_driver.h"

/** Compare a byte buffer against a literal that may contain NULs. */
static bool binary_equal(const uint8_t *actual, const char *expected, size_t len)
{
    return memcmp(actual, expected, len) == 0;
}

/* ============================================================ the bytes == */

static void test_devices(void)
{
    uint8_t code = 0;
    ncl_mc_unit unit = NCL_MC_BIT;

    NCL_TEST_CASE("device codes and their unit");
    NCL_CHECK(ncl_mc_device_lookup("D", &code, &unit) && code == 0xA8 &&
              unit == NCL_MC_WORD);
    NCL_CHECK(ncl_mc_device_lookup("m", &code, &unit) && code == 0x90 &&
              unit == NCL_MC_BIT);
    NCL_CHECK(ncl_mc_device_lookup("X", &code, &unit) && code == 0x9C);
    NCL_CHECK(ncl_mc_device_lookup("TN", &code, &unit) && code == 0xC2 &&
              unit == NCL_MC_WORD);
    NCL_CHECK(!ncl_mc_device_lookup("Q", &code, &unit));
    NCL_CHECK_EQ_STR(ncl_mc_device_name(0xAF), "R");
    NCL_CHECK(ncl_mc_device_name(0x01) == NULL);

    NCL_TEST_CASE("a word device with a bit is addressed number * 16 + bit");
    NCL_CHECK_EQ_INT(ncl_mc_wire_address(100, -1, true), 100);
    NCL_CHECK_EQ_INT(ncl_mc_wire_address(100, 3, true), 1603);
    /* M100 keeps its plain number: the bit sub command already says "in bits" */
    NCL_CHECK_EQ_INT(ncl_mc_wire_address(100, -1, false), 100);
    NCL_CHECK_EQ_INT(ncl_mc_wire_address(100, 3, false), 100);
}

static void test_requests(void)
{
    ncl_mc_header header;
    uint8_t spec[8];
    uint8_t frame[64];
    size_t len;

    NCL_TEST_CASE("the 3E read of D100 for 4 words matches the spec book");
    ncl_mc_header_default(&header, NCL_MC_FRAME_3E);
    NCL_CHECK_EQ_INT(header.plc, 0xFF);
    NCL_CHECK_EQ_INT(header.module, 0x03FF);
    NCL_CHECK_EQ_INT(header.timer, 0x0010);
    NCL_CHECK_EQ_INT(ncl_mc_device_spec(spec, sizeof(spec), 0xA8, 100, 4), 6);
    NCL_CHECK(binary_equal(spec, "\xA8\x64\x00\x00\x04\x00", 6));
    len = ncl_mc_request(frame, sizeof(frame), &header, NCL_MC_CMD_BATCH_READ,
                         NCL_MC_SUB_WORD, spec, 6);
    NCL_CHECK_EQ_INT(len, 21);
    NCL_CHECK(binary_equal(frame,
                           "\xD0\x00\x00\xFF\xFF\x03\x00\x0C\x00\x10\x00"
                           "\x01\x04\x00\x00\xA8\x64\x00\x00\x04\x00",
                           21));

    NCL_TEST_CASE("a 4E request carries its serial number after the sub header");
    ncl_mc_header_default(&header, NCL_MC_FRAME_4E);
    header.serial = 0x1234;
    len = ncl_mc_request(frame, sizeof(frame), &header, NCL_MC_CMD_LOOPBACK,
                         0x0000, NULL, 0);
    NCL_CHECK_EQ_INT(len, 17);
    NCL_CHECK(binary_equal(frame, "\xD0\x00\x34\x12\x00\xFF\xFF\x03\x00\x06\x00"
                                  "\x10\x00\x19\x06\x00\x00",
                           17));
}

static void test_replies(void)
{
    ncl_mc_reply reply;
    size_t frame_len = 0;
    char message[160];

    NCL_TEST_CASE("a reply splits into end code and data");
    {
        static const uint8_t kReply[] = {0xD0, 0x00, 0x00, 0xFF, 0xFF, 0x03,
                                         0x00, 0x06, 0x00, 0x00, 0x00,
                                         0xE8, 0x03, 0x64, 0x00};

        NCL_CHECK_EQ_INT(ncl_mc_split_reply(kReply, sizeof(kReply),
                                            NCL_MC_FRAME_3E, 0, &reply,
                                            &frame_len),
                         NCL_OK);
        NCL_CHECK_EQ_INT(reply.end_code, 0);
        NCL_CHECK_EQ_INT(reply.data_len, 4);
        NCL_CHECK_EQ_INT(frame_len, sizeof(kReply));
        NCL_CHECK_EQ_INT(reply.data[0], 0xE8); /* 1000, little endian */
        /* a frame that is still arriving is not an error, just incomplete */
        NCL_CHECK_EQ_INT(ncl_mc_split_reply(kReply, 8, NCL_MC_FRAME_3E, 0,
                                            &reply, NULL),
                         NCL_ERR_RANGE);
        /* and something that is not a 3E/4E frame is a protocol error */
        NCL_CHECK(ncl_driver_error_tier(ncl_mc_split_reply(
                       kReply + 1, sizeof(kReply) - 1, NCL_MC_FRAME_3E, 0,
                       &reply, NULL)) == 2);
    }

    NCL_TEST_CASE("a 4E reply must carry our serial number back");
    {
        static const uint8_t kReply[] = {0xD0, 0x00, 0x34, 0x12, 0x00, 0xFF,
                                         0xFF, 0x03, 0x00, 0x04, 0x00, 0x00,
                                         0x00, 0xE8, 0x03};

        NCL_CHECK_EQ_INT(ncl_mc_split_reply(kReply, sizeof(kReply),
                                            NCL_MC_FRAME_4E, 0x1234, &reply,
                                            NULL),
                         NCL_OK);
        NCL_CHECK_EQ_INT(reply.end_code, 0);
        NCL_CHECK_EQ_INT(reply.data_len, 2);
        NCL_CHECK(ncl_driver_error_tier(ncl_mc_split_reply(
                       kReply, sizeof(kReply), NCL_MC_FRAME_4E, 0x9999, &reply,
                       NULL)) == 2);
    }

    NCL_TEST_CASE("end codes become tiered, readable errors");
    NCL_CHECK_EQ_INT(ncl_mc_check_end_code(0x0000, message, sizeof(message)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_mc_check_end_code(0xC050, message, sizeof(message)),
                     NCL_DRV_ERR_PROTOCOL(0x150));
    NCL_CHECK(strstr(message, "地址越界") != NULL);
    NCL_CHECK_EQ_INT(ncl_mc_check_end_code(0xC059, message, sizeof(message)),
                     NCL_DRV_ERR_PROTOCOL(0x159));
    /* a CPU in a fault state is the device's business, not the frame's */
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_mc_check_end_code(
                         0xC0B5, message, sizeof(message))),
                     3);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_mc_check_end_code(
                         0x9999, message, sizeof(message))),
                     2);
}

static void test_data(void)
{
    ncl_json *value = NULL;
    uint8_t bytes[8];
    size_t len = 0;

    NCL_TEST_CASE("data words are little endian");
    memcpy(bytes, "\xE8\x03", 2); /* 1000 */
    NCL_CHECK_EQ_INT(ncl_mc_decode(bytes, 2, 0, NCL_DTYPE_INT16, 0, &value),
                     NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 1000);
    }
    ncl_json_free(value);

    NCL_TEST_CASE("a 32 bit value is low word first");
    memcpy(bytes, "\x00\x00\x00\x00", 4);
    bytes[0] = 0x39;
    bytes[1] = 0x30; /* low word  0x3039 */
    bytes[2] = 0x01; /* high word 0x0001 */
    bytes[3] = 0x00;
    NCL_CHECK_EQ_INT(ncl_mc_decode(bytes, 4, 0, NCL_DTYPE_INT32, 0, &value),
                     NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 0x00013039);
    }
    ncl_json_free(value);

    NCL_TEST_CASE("1.5f decodes the way the PLC stores it");
    bytes[0] = 0x00;
    bytes[1] = 0x00;
    bytes[2] = 0xC0;
    bytes[3] = 0x3F; /* high word 0x3FC0, low word 0x0000 */
    NCL_CHECK_EQ_INT(ncl_mc_decode(bytes, 4, 0, NCL_DTYPE_FLOAT32, 0, &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
    }
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(ncl_mc_encode(ncl_json_new_double(1.5), NCL_DTYPE_FLOAT32,
                                   0, bytes, sizeof(bytes), &len),
                     NCL_OK);
    NCL_CHECK_EQ_INT(len, 4);
    NCL_CHECK(binary_equal(bytes, "\x00\x00\xC0\x3F", 4));

    NCL_TEST_CASE("a string is two characters per word, low byte first");
    memcpy(bytes, "ABCD", 4);
    NCL_CHECK_EQ_INT(ncl_mc_decode(bytes, 4, 0, NCL_DTYPE_STRING, 4, &value),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "ABCD");
    ncl_json_free(value);
}

/* ============================================================= the wire == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    bool        four_e;       /**< answer with 4E frames */
    int         requests;
    int         fails_to_skip;
    uint16_t    words[256];   /**< the D area   */
    uint8_t     bits[32];     /**< the M area   */
} mc_slave;

static size_t mc_slave_reply(mc_slave *s, const uint8_t *pdu, size_t pdu_len,
                             uint8_t *out, size_t cap)
{
    uint16_t command = (uint16_t)(pdu[0] | ((uint16_t)pdu[1] << 8));
    uint16_t subcommand = (uint16_t)(pdu[2] | ((uint16_t)pdu[3] << 8));
    const uint8_t *data = pdu + 4;
    size_t data_len = pdu_len - 4;
    uint8_t device = data_len >= 1 ? data[0] : 0;
    uint32_t address = data_len >= 4 ? (uint32_t)(data[1] | (data[2] << 8) |
                                                  ((uint32_t)data[3] << 16))
                                     : 0;
    uint16_t points = data_len >= 6 ? (uint16_t)(data[4] | (data[5] << 8)) : 0;
    uint16_t end_code = 0;
    size_t used = 0;

    s->requests++;
    switch (command) {
    case NCL_MC_CMD_BATCH_READ:
        if (device == 0xA8) { /* D, one word per point */
            if (subcommand == NCL_MC_SUB_BIT) {
                /* a bit of a word register: the wire address is word*16+bit */
                uint32_t word = address / 16u;
                uint32_t bit = address % 16u;

                if (word >= 256) {
                    end_code = 0xC050;
                    break;
                }
                out[used++] = (uint8_t)((s->words[word] >> bit) & 1u);
            } else {
                uint16_t i;

                if (address + points > 256) {
                    end_code = 0xC050;
                    break;
                }
                for (i = 0; i < points; i++) {
                    out[used++] = (uint8_t)(s->words[address + i] & 0xFF);
                    out[used++] = (uint8_t)(s->words[address + i] >> 8);
                }
            }
        } else if (device == 0x90 || device == 0x9C || device == 0x9D) {
            /* M/X/Y: one byte per point, plain device numbers */
            uint32_t i;

            if (subcommand != NCL_MC_SUB_BIT) {
                end_code = 0xC051; /* these are bit devices */
                break;
            }
            if (address + points > 256) {
                end_code = 0xC050;
                break;
            }
            for (i = 0; i < points; i++) {
                uint32_t bit = address + i;

                out[used++] = (uint8_t)((s->bits[bit / 8u] >> (bit % 8u)) & 1u);
            }
        } else {
            end_code = 0xC059; /* no such device */
        }
        break;
    case NCL_MC_CMD_BATCH_WRITE:
        if (device == 0xA8) {
            uint16_t i;

            if (address + points > 256 || data_len < 6u + (size_t)points * 2u) {
                end_code = 0xC050;
                break;
            }
            for (i = 0; i < points; i++) {
                s->words[address + i] =
                    (uint16_t)(data[6 + i * 2] | (data[7 + i * 2] << 8));
            }
        } else if (device == 0x90) {
            uint32_t i;

            if (address + points > 256 || data_len < 6u + points) {
                end_code = 0xC050;
                break;
            }
            for (i = 0; i < points; i++) {
                uint32_t bit = address + i;

                if (data[6 + i] != 0) {
                    s->bits[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
                } else {
                    s->bits[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
                }
            }
        } else {
            end_code = 0xC059;
        }
        break;
    case NCL_MC_CMD_LOOPBACK:
        if (data_len > cap) {
            data_len = cap;
        }
        memcpy(out, data, data_len);
        used = data_len;
        break;
    default:
        end_code = 0xC059; /* command not found */
        break;
    }
    /* The reply carries the end code first, then the data. */
    if (used > 0) {
        memmove(out + 2, out, used);
    }
    out[0] = (uint8_t)(end_code & 0xFF);
    out[1] = (uint8_t)(end_code >> 8);
    return used + 2; /* the caller prepends the frame header */
}

static void mc_slave_main(void *arg)
{
    mc_slave *s = (mc_slave *)arg;

    while (!s->stop) {
        ncl_socket *peer = ncl_socket_accept(s->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t head[16];
            uint8_t payload[2048];
            uint8_t data[2048];
            uint8_t frame[2100];
            size_t base = s->four_e ? 2u : 0u; /* the serial number, if any */
            size_t prefix = 9u + base;
            size_t length;
            size_t reply_len;
            size_t frame_len;

            if (ncl_socket_recv_exact(peer, head, prefix, 2000) != NCL_OK) {
                break;
            }
            length = (size_t)(head[7 + base] | ((uint16_t)head[8 + base] << 8));
            if (length < 4 || length > sizeof(payload) + 2) {
                break;
            }
            if (ncl_socket_recv_exact(peer, payload, length, 2000) != NCL_OK) {
                break;
            }
            if (s->fails_to_skip > 0) {
                s->fails_to_skip--;
                ncl_sleep_millis(200);
                break;
            }
            /* payload = timer(2) + command(2) + sub command(2) + data */
            reply_len = mc_slave_reply(s, payload + 2, length - 2, data,
                                       sizeof(data));
            /* sub header + [serial] + the echoed header + length + end code
             * and data */
            frame_len = 0;
            frame[frame_len++] = 0xD0;
            frame[frame_len++] = 0x00;
            if (s->four_e) {
                frame[frame_len++] = head[2]; /* the serial, echoed */
                frame[frame_len++] = head[3];
            }
            frame[frame_len++] = head[2 + base]; /* network   */
            frame[frame_len++] = head[3 + base]; /* plc       */
            frame[frame_len++] = head[4 + base]; /* module    */
            frame[frame_len++] = head[5 + base];
            frame[frame_len++] = head[6 + base]; /* station   */
            frame[frame_len++] = (uint8_t)(reply_len & 0xFF);
            frame[frame_len++] = (uint8_t)(reply_len >> 8);
            memcpy(frame + frame_len, data, reply_len);
            frame_len += reply_len;
            if (ncl_socket_send(peer, frame, frame_len) != NCL_OK) {
                break;
            }
        }
        ncl_socket_close(peer);
    }
}

static mc_slave *mc_slave_start(bool four_e)
{
    mc_slave *s = (mc_slave *)ncl_mem_calloc(1, sizeof(*s));
    size_t i;

    if (s == NULL) {
        return NULL;
    }
    s->four_e = four_e;
    s->listener = ncl_socket_listen(0, NULL, 0);
    if (s->listener == NULL) {
        ncl_free_safe(s);
        return NULL;
    }
    s->port = ncl_socket_local_port(s->listener);
    for (i = 0; i < 256; i++) {
        s->words[i] = (uint16_t)(500 + i);
    }
    s->thread = ncl_thread_start(mc_slave_main, s);
    if (s->thread == NULL) {
        ncl_socket_close(s->listener);
        ncl_free_safe(s);
        return NULL;
    }
    return s;
}

static void mc_slave_stop(mc_slave *s)
{
    if (s == NULL) {
        return;
    }
    s->stop = true;
    ncl_thread_join(s->thread);
    ncl_socket_close(s->listener);
    ncl_free_safe(s);
}

static ncl_driver *mc_driver(mc_slave *s, const char *extra)
{
    ncl_driver *driver = ncl_mc_tcp_create();
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
    mc_slave *s = mc_slave_start(false);
    ncl_driver *driver;
    ncl_err err = NCL_ERR;
    long long number;

    NCL_TEST_CASE("a real socket: reading D registers");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = mc_driver(s, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mc_slave_stop(s);
        return;
    }
    number = read_int(driver, "\"D10\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 510);

    NCL_TEST_CASE("writing a register and reading it back");
    {
        ncl_address address;

        NCL_CHECK_EQ_INT(parse_address("\"D20\"", &address), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address,
                                              ncl_json_new_int(4242)),
                         NCL_OK);
        ncl_address_clear(&address);
        NCL_CHECK_EQ_INT(s->words[20], 4242);
    }
    number = read_int(driver, "\"D20\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 4242);

    NCL_TEST_CASE("a bit device reads and writes one point per byte");
    {
        ncl_address address;
        ncl_json *value = NULL;
        bool set = false;

        NCL_CHECK_EQ_INT(parse_address("\"M13\"", &address), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address,
                                              ncl_json_new_bool(true)),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->bits[1], 0x20); /* M13 = byte 1, bit 5 */
        NCL_CHECK_EQ_INT(ncl_driver_read_one(driver, &address, &value), NCL_OK);
        NCL_CHECK(ncl_json_as_bool(value, &set) && set);
        ncl_json_free(value);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("a word's bit is addressed as number * 16 + bit");
    {
        ncl_address address;
        ncl_json *value = NULL;
        bool set = false;

        s->words[30] = 0x0004; /* bit 2 of D30 */
        NCL_CHECK_EQ_INT(parse_address("\"D30.2\"", &address), NCL_OK);
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
            NCL_CHECK_EQ_INT(last, 507);
        }
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);

        before = s->requests;
        NCL_CHECK_EQ_INT(parse_address("\"D0\"", &addresses[0]), NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("\"D200\"", &addresses[1]), NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 2, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->requests - before, 2);
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);
    }

    NCL_TEST_CASE("the device's own end code becomes a protocol error");
    number = read_int(driver, "\"D2000\"", &err);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 2);
    NCL_CHECK_EQ_INT(number, -1);

    NCL_TEST_CASE("the loopback method echoes what it sent");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "loopback", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "bytes", -1), 6);
        ncl_json_free(result);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("the raw hatch sends a command and returns the data");
    {
        ncl_driver_result out;
        /* command 0x0619 (loopback) + sub command + 2 data bytes */
        const uint8_t raw[] = {0x19, 0x06, 0x00, 0x00, 0xAA, 0x55};

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, raw, sizeof(raw), &out),
                         NCL_OK);
        NCL_CHECK_EQ_STR(ncl_json_as_string(out.value), "aa55");
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("the session reopens on demand");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    number = read_int(driver, "\"D1\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 501);

    driver->ops->destroy(driver);
    mc_slave_stop(s);
}

static void test_four_e_and_silence(void)
{
    mc_slave *s = mc_slave_start(true);
    ncl_driver *driver;
    ncl_err err = NCL_ERR;

    NCL_TEST_CASE("a 4E link talks to a 4E device");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = mc_driver(s, ",\"frame\":\"4e\"");
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        NCL_CHECK_EQ_INT(read_int(driver, "\"D5\"", &err), 505);
        NCL_CHECK_EQ_INT(err, NCL_OK);
        /* the serial number advances with every request */
        NCL_CHECK_EQ_INT(read_int(driver, "\"D6\"", &err), 506);
        driver->ops->destroy(driver);
    }
    mc_slave_stop(s);

    NCL_TEST_CASE("a device that stops answering is a transport failure");
    s = mc_slave_start(false);
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = mc_driver(s, NULL);
    if (driver != NULL) {
        s->fails_to_skip = 1;
        (void)read_int(driver, "\"D0\"", &err);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);
        driver->ops->destroy(driver);
    }
    mc_slave_stop(s);
}

static void test_through_the_manager(void)
{
    mc_slave *s = mc_slave_start(false);
    test_point_map *manager = test_point_map_create(ncl_mc_tcp_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured MC link reads through the point map");
    NCL_CHECK(s != NULL && manager != NULL);
    if (s == NULL || manager == NULL) {
        mc_slave_stop(s);
        test_point_map_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"plc\",\"path\":\"/PLC\","
                            "\"type\":\"mc_tcp\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":600},"
                            "\"points\":["
                            "{\"path\":\"/PLC/TEMP\",\"addr\":\"D32\"},"
                            "{\"path\":\"/PLC/READY\",\"addr\":\"M3\"}]}",
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

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/TEMP", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 532);
    ncl_json_free(value);
    value = NULL;

    s->bits[0] = 0x08; /* M3 on */
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/READY", &value),
                     NCL_OK);
    {
        bool set = false;

        NCL_CHECK(ncl_json_as_bool(value, &set) && set);
    }
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    mc_slave_stop(s);
}

NCL_TEST_MAIN_BEGIN()
    test_devices();
    test_requests();
    test_replies();
    test_data();
    test_against_a_plc();
    test_four_e_and_silence();
    test_through_the_manager();
NCL_TEST_MAIN_END()
