/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Siemens S7: the byte level against 03-SIEMENS-S7-PLC.md §5, then a real
 * socket against a mock PLC that speaks TPKT + COTP + S7comm, handshake and
 * all.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "test_point_map.h"
#include "nclink/clients/s7.h"
#include "s7/ncl_s7_driver.h"

static bool binary_equal(const uint8_t *actual, const char *expected, size_t len)
{
    return memcmp(actual, expected, len) == 0;
}

/* ============================================================ the bytes == */

static void test_areas(void)
{
    uint8_t area = 0;
    uint16_t db = 0;
    ncl_dtype implied = NCL_DTYPE_INT16;

    NCL_TEST_CASE("area names, DB numbers and the width suffixes");
    NCL_CHECK(ncl_s7_area_lookup("I", &area, &db, &implied) && area == 0x81);
    NCL_CHECK(ncl_s7_area_lookup("E", &area, &db, NULL) && area == 0x81);
    NCL_CHECK(ncl_s7_area_lookup("Q", &area, &db, NULL) && area == 0x82);
    NCL_CHECK(ncl_s7_area_lookup("A", &area, &db, NULL) && area == 0x82);
    NCL_CHECK(ncl_s7_area_lookup("M", &area, &db, NULL) && area == 0x83);
    NCL_CHECK(ncl_s7_area_lookup("T", &area, &db, NULL) && area == 0x1D);
    NCL_CHECK(ncl_s7_area_lookup("C", &area, &db, NULL) && area == 0x1C);
    NCL_CHECK(ncl_s7_area_lookup("DB1", &area, &db, NULL) && area == 0x84 &&
              db == 1);
    NCL_CHECK(ncl_s7_area_lookup("db100", &area, &db, NULL) && db == 100);
    NCL_CHECK(!ncl_s7_area_lookup("DB", &area, &db, NULL)); /* no number */
    NCL_CHECK(!ncl_s7_area_lookup("DBX", &area, &db, NULL));
    NCL_CHECK(!ncl_s7_area_lookup("X", &area, &db, NULL));
    /* MB/MW/MD name the area and the access width at once (§4) */
    NCL_CHECK(ncl_s7_area_lookup("MB", &area, &db, &implied) && area == 0x83 &&
              implied == NCL_DTYPE_BYTE);
    NCL_CHECK(ncl_s7_area_lookup("MW", &area, &db, &implied) &&
              implied == NCL_DTYPE_INT16);
    NCL_CHECK(ncl_s7_area_lookup("MD", &area, &db, &implied) &&
              implied == NCL_DTYPE_INT32);
    NCL_CHECK(ncl_s7_area_lookup("IB", &area, &db, &implied) && area == 0x81);
}

static void test_frames(void)
{
    uint8_t frame[64];
    uint8_t pdu[64];
    uint8_t params[16];
    size_t len;
    size_t pdu_len;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t pdu_type = 0;
    uint8_t pdu_code = 0;
    ncl_s7_view view;

    NCL_TEST_CASE("the COTP connection request of §2");
    NCL_CHECK_EQ_INT(ncl_s7_tsap(0, 2), 0x0302); /* S7-300/400 */
    NCL_CHECK_EQ_INT(ncl_s7_tsap(0, 1), 0x0301); /* S7-1200/1500 */
    NCL_CHECK_EQ_INT(ncl_s7_tsap(1, 2), 0x0322);
    len = ncl_s7_cotp_cr(frame, sizeof(frame), 0x0100, ncl_s7_tsap(0, 2),
                         NCL_S7_PDU_1024);
    NCL_CHECK_EQ_INT(len, 22);
    NCL_CHECK(binary_equal(frame, "\x03\x00\x00\x16\x11\xE0\x00\x00\x00\x01"
                                  "\x00\xC1\x02\x01\x00\xC2\x02\x03\x02"
                                  "\xC0\x01\x0A",
                           22));
    {
        const uint8_t *cotp = NULL;
        size_t cotp_len = 0;

        NCL_CHECK_EQ_INT(ncl_s7_tpkt_split(frame, len, &cotp, &cotp_len, NULL),
                         NCL_OK);
        NCL_CHECK_EQ_INT(cotp_len, 18);
        NCL_CHECK_EQ_INT(ncl_s7_cotp_split(cotp, cotp_len, &pdu_type, &pdu_code,
                                           NULL, NULL, NULL),
                         NCL_OK);
    }
    NCL_CHECK_EQ_INT(pdu_type, NCL_S7_COTP_CR);
    NCL_CHECK_EQ_INT(pdu_code, NCL_S7_PDU_1024);

    NCL_TEST_CASE("Setup Communication: 8 parameters, 0x03C0 asked for");
    len = ncl_s7_setup_params(params, sizeof(params), 1, 1, 960);
    NCL_CHECK_EQ_INT(len, 8);
    NCL_CHECK(binary_equal(params, "\xF0\x00\x00\x01\x00\x01\x03\xC0", 8));
    pdu_len = ncl_s7_pdu(pdu, sizeof(pdu), NCL_S7_ROSCTR_JOB, 1, params, 8, NULL,
                         0);
    NCL_CHECK_EQ_INT(pdu_len, 18);
    NCL_CHECK(binary_equal(pdu, "\x32\x01\x00\x00\x00\x01\x00\x08\x00\x00"
                                "\xF0\x00\x00\x01\x00\x01\x03\xC0",
                           18));
    len = ncl_s7_cotp_dt(frame, sizeof(frame), pdu, pdu_len);
    NCL_CHECK_EQ_INT(len, 25);
    NCL_CHECK(binary_equal(frame, "\x03\x00\x00\x19\x02\xF0\x80", 7));

    NCL_TEST_CASE("the reply splits back into PDU, parameters and data");
    {
        static const uint8_t kReply[] = {
            0x03, 0x00, 0x00, 0x19, 0x02, 0xF0, 0x80,
            0x32, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00,
            0xF0, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0,
        };
        uint16_t pdu_size = 0;

        NCL_CHECK_EQ_INT(ncl_s7_tpkt_split(kReply, sizeof(kReply), &payload,
                                           &payload_len, NULL),
                         NCL_OK);
        NCL_CHECK_EQ_INT(payload_len, sizeof(kReply) - 4);
        NCL_CHECK_EQ_INT(ncl_s7_cotp_split(payload, payload_len, &pdu_type, NULL,
                                           &payload, &payload_len, NULL),
                         NCL_OK);
        NCL_CHECK_EQ_INT(pdu_type, NCL_S7_COTP_DT);
        NCL_CHECK_EQ_INT(ncl_s7_pdu_split(payload, payload_len, &view, NULL),
                         NCL_OK);
        NCL_CHECK_EQ_INT(view.rosctr, NCL_S7_ROSCTR_ACK_DATA);
        NCL_CHECK_EQ_INT(view.pdu_ref, 1);
        NCL_CHECK_EQ_INT(ncl_s7_setup_reply(view.params, view.params_len,
                                            &pdu_size),
                         NCL_OK);
        NCL_CHECK_EQ_INT(pdu_size, 480); /* the PLC's own ceiling */
    }

    NCL_TEST_CASE("a truncated frame is not an error, just incomplete");
    NCL_CHECK_EQ_INT(ncl_s7_tpkt_split(frame, 2, NULL, NULL, NULL), NCL_ERR_RANGE);
    NCL_CHECK_EQ_INT(ncl_s7_pdu_split(pdu, 5, &view, NULL), NCL_ERR_RANGE);
}

static void test_variables(void)
{
    ncl_s7_item items[2];
    uint8_t params[64];
    uint8_t data[32];
    size_t len;
    size_t byte_len = 0;
    ncl_address address;
    ncl_json *node;

    NCL_TEST_CASE("a REAL read of DB1.DBD0 is the frame of §5");
    node = ncl_json_parse_cstr("{\"area\":\"DB1\",\"offset\":0,"
                               "\"dtype\":\"float32\"}",
                               NULL);
    NCL_CHECK_EQ_INT(ncl_address_from_json(node, &address), NCL_OK);
    ncl_json_free(node);
    NCL_CHECK(ncl_s7_item_for(&address, &items[0], &byte_len));
    NCL_CHECK_EQ_INT(byte_len, 4);
    NCL_CHECK_EQ_INT(items[0].area, 0x84);
    NCL_CHECK_EQ_INT(items[0].db, 1);
    NCL_CHECK_EQ_INT(items[0].tsize, NCL_S7_TS_REAL);
    /* the length field counts elements of the transport size: one REAL */
    NCL_CHECK_EQ_INT(items[0].elements, 1);
    NCL_CHECK_EQ_INT(items[0].bit, 0);
    ncl_address_clear(&address);
    len = ncl_s7_var_params(params, sizeof(params), NCL_S7_FUNC_READ_VAR, items,
                            1);
    NCL_CHECK_EQ_INT(len, 14);
    NCL_CHECK(binary_equal(params, "\x04\x01\x12\x0A\x10\x08\x00\x01\x00\x01"
                                   "\x84\x00\x00\x00",
                           14));

    NCL_TEST_CASE("a bit of M is a bit access");
    node = ncl_json_parse_cstr("\"M10.0\"", NULL);
    NCL_CHECK_EQ_INT(ncl_address_from_json(node, &address), NCL_OK);
    ncl_json_free(node);
    NCL_CHECK(ncl_s7_item_for(&address, &items[0], &byte_len));
    NCL_CHECK_EQ_INT(items[0].area, 0x83);
    NCL_CHECK_EQ_INT(items[0].tsize, NCL_S7_TS_BIT);
    NCL_CHECK_EQ_INT(items[0].bit, 80); /* byte 10, bit 0 */
    NCL_CHECK_EQ_INT(items[0].elements, 1);
    ncl_address_clear(&address);

    NCL_TEST_CASE("a write item carries its length in bits");
    len = ncl_s7_write_data(data, sizeof(data), NCL_S7_TS_REPLY_BIT, 1,
                            (const uint8_t *)"\x01", 1);
    NCL_CHECK_EQ_INT(len, 6); /* reserved + tsize + bits + one padded byte */
    NCL_CHECK(binary_equal(data, "\x00\x03\x00\x01\x01\x00", 6));

    NCL_TEST_CASE("a read reply walks its data items");
    {
        static const uint8_t kData[] = {
            0xFF, 0x04, 0x00, 0x20, /* 32 bits of byte oriented data */
            0x3F, 0xC0, 0x00, 0x00,
            0x0A, 0x00, 0x00, 0x00, /* the second item: no such object */
        };
        ncl_s7_reply_item replies[2];
        size_t filled = 0;
        char message[160];

        NCL_CHECK_EQ_INT(ncl_s7_read_reply(kData, sizeof(kData), 2, replies,
                                           &filled),
                         NCL_OK);
        NCL_CHECK_EQ_INT(filled, 2);
        NCL_CHECK_EQ_INT(replies[0].return_code, 0xFF);
        NCL_CHECK_EQ_INT(replies[0].byte_len, 4);
        NCL_CHECK_EQ_INT(replies[0].bits, 32);
        NCL_CHECK(replies[0].bytes[0] == 0x3F);
        NCL_CHECK_EQ_INT(ncl_s7_check_return_code(replies[0].return_code, message,
                                                  sizeof(message)),
                         NCL_OK);
        /* a missing DB is the classic 0x0A */
        NCL_CHECK_EQ_INT(ncl_s7_check_return_code(replies[1].return_code, message,
                                                  sizeof(message)),
                         NCL_DRV_ERR_BUSINESS(0x30A));
        NCL_CHECK(strstr(message, "对象不存在") != NULL);
        /* an address that does not exist is the frame's fault, not the device's */
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_s7_check_return_code(
                             0x05, message, sizeof(message))),
                         2);
    }

    NCL_TEST_CASE("strings carry S7's two byte header");
    {
        uint8_t text[8];
        size_t written = 0;
        ncl_json *value = NULL;

        NCL_CHECK_EQ_INT(ncl_s7_encode(ncl_json_new_string("AB"),
                                       NCL_DTYPE_STRING, 4, text, sizeof(text),
                                       &written),
                         NCL_OK);
        NCL_CHECK_EQ_INT(written, 6);
        NCL_CHECK(text[0] == 4 && text[1] == 2 && text[2] == 'A' &&
                  text[3] == 'B');
        NCL_CHECK_EQ_INT(ncl_s7_decode(text, written, 0, NCL_DTYPE_STRING, 4,
                                       &value),
                         NCL_OK);
        NCL_CHECK_EQ_STR(ncl_json_as_string(value), "AB  ");
        ncl_json_free(value);
    }
}

/* ============================================================= the wire == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    int         requests;
    int         fails_to_skip;
    int         connections; /**< COTP connection requests seen */
    uint16_t    pdu_size;    /**< what the PLC agrees to */
    uint8_t     flags[64];   /**< M area   */
    uint8_t     db1[64];     /**< DB1      */
} s7_slave;

/** Answer an S7 PDU: returns the reply PDU length. */
static size_t s7_slave_pdu(s7_slave *s, const uint8_t *pdu, size_t pdu_len,
                           uint8_t *out, size_t cap)
{
    uint8_t function = pdu_len > 10 ? pdu[10] : 0;
    uint16_t reference = (uint16_t)((pdu[4] << 8) | pdu[5]);
    uint8_t params[512];
    uint8_t data[1024];
    size_t params_len = 0;
    size_t data_len = 0;

    s->requests++;
    switch (function) {
    case NCL_S7_FUNC_SETUP_COMM:
        params_len = ncl_s7_setup_params(params, sizeof(params), 1, 1,
                                         s->pdu_size);
        break;
    case NCL_S7_FUNC_READ_VAR: {
        size_t count = pdu_len > 11 ? pdu[11] : 0;
        size_t at = 12;
        size_t i;

        params_len = 2; /* function + item count */
        params[0] = function;
        params[1] = 0;
        for (i = 0; i < count && at + NCL_S7_ITEM_BYTES <= pdu_len; i++) {
            uint8_t tsize = pdu[at + 3];
            uint16_t elements = (uint16_t)((pdu[at + 4] << 8) | pdu[at + 5]);
            uint16_t db = (uint16_t)((pdu[at + 6] << 8) | pdu[at + 7]);
            uint8_t area = pdu[at + 8];
            uint32_t bit = ((uint32_t)pdu[at + 9] << 16) |
                           ((uint32_t)pdu[at + 10] << 8) | pdu[at + 11];
            size_t byte_offset = bit / 8u;
            /* the length field counts elements of the transport size */
            size_t bytes = tsize == NCL_S7_TS_BYTE || tsize == NCL_S7_TS_CHAR
                               ? (size_t)elements
                           : tsize == NCL_S7_TS_WORD || tsize == NCL_S7_TS_INT
                               ? (size_t)elements * 2u
                           : tsize == NCL_S7_TS_DWORD ||
                                     tsize == NCL_S7_TS_DINT ||
                                     tsize == NCL_S7_TS_REAL
                               ? (size_t)elements * 4u
                               : (size_t)elements;
            const uint8_t *source = NULL;
            size_t j;

            /* the reply counts bits, and pads to an even byte count */
            if (tsize == NCL_S7_TS_BIT) {
                data[data_len++] = 0xFF;
                data[data_len++] = NCL_S7_TS_REPLY_BIT;
                data[data_len++] = (uint8_t)(elements >> 8);
                data[data_len++] = (uint8_t)elements;
                source = area == 0x83 ? s->flags + byte_offset
                                      : s->db1 + byte_offset;
                for (j = 0; j < elements; j++) {
                    data[data_len++] =
                        (uint8_t)((source[j / 8u] >> (j % 8u)) & 1u);
                }
                if (elements % 2u != 0) {
                    data[data_len++] = 0;
                }
            } else if ((area == 0x83 || (area == 0x84 && db == 1) ||
                        (area == 0x81 && db == 0)) &&
                       byte_offset + bytes <= 64) {
                data[data_len++] = 0xFF;
                data[data_len++] = NCL_S7_TS_REPLY_BYTE;
                data[data_len++] = (uint8_t)((bytes * 8u) >> 8);
                data[data_len++] = (uint8_t)(bytes * 8u);
                source = area == 0x84 ? s->db1 + byte_offset
                                      : s->flags + byte_offset;
                for (j = 0; j < bytes; j++) {
                    data[data_len++] = source[j];
                }
                if (bytes % 2u != 0) {
                    data[data_len++] = 0;
                }
            } else {
                data[data_len++] = 0x0A; /* object does not exist */
                data[data_len++] = 0x00;
                data[data_len++] = 0x00;
                data[data_len++] = 0x00;
            }
            at += NCL_S7_ITEM_BYTES;
        }
        break;
    }
    case NCL_S7_FUNC_WRITE_VAR: {
        uint8_t area = pdu_len > 22 ? pdu[20] : 0;
        uint16_t db = (uint16_t)((pdu[18] << 8) | pdu[19]);
        uint32_t bit = ((uint32_t)pdu[21] << 16) | ((uint32_t)pdu[22] << 8) |
                       (pdu_len > 23 ? pdu[23] : 0);
        size_t byte_offset = bit / 8u;
        size_t at = 10u + ((size_t)pdu[6] << 8) + pdu[7]; /* the data area */
        uint16_t bits = (uint16_t)(((pdu[at + 2] << 8) | pdu[at + 3]));
        size_t bytes = (bits + 7u) / 8u;
        uint8_t *target = area == 0x83 ? s->flags : s->db1;

        (void)db;
        if (byte_offset + bytes <= 64) {
            memcpy(target + byte_offset, pdu + at + 4, bytes);
        }
        data[data_len++] = 0xFF; /* one return code per written item */
        params_len = 2;
        params[0] = function;
        params[1] = 1;
        break;
    }
    default:
        params_len = 1;
        params[0] = 0x00; /* not a command this PLC knows */
        break;
    }
    return ncl_s7_pdu(out, cap, NCL_S7_ROSCTR_ACK_DATA, reference, params,
                      params_len, data, data_len);
}

static void s7_slave_main(void *arg)
{
    s7_slave *s = (s7_slave *)arg;

    while (!s->stop) {
        ncl_socket *peer = ncl_socket_accept(s->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t head[4];
            uint8_t payload[2048];
            uint8_t s7[2048];
            uint8_t frame[2048];
            size_t total;
            size_t payload_len;

            if (ncl_socket_recv_exact(peer, head, 4, 2000) != NCL_OK) {
                break;
            }
            total = ((size_t)head[2] << 8) | head[3];
            if (total < 4 || total > sizeof(payload) + 4) {
                break;
            }
            if (ncl_socket_recv_exact(peer, payload, total - 4u, 2000) != NCL_OK) {
                break;
            }
            payload_len = total - 4u;
            if (s->fails_to_skip > 0) {
                s->fails_to_skip--;
                ncl_sleep_millis(200);
                break;
            }
            if (payload_len >= 2 && payload[1] == NCL_S7_COTP_CR) {
                /* connection confirm: the same shape, with our PDU size */
                static const uint8_t kCc[] = {0x03, 0x00, 0x00, 0x16,
                                              0x11, 0xD0, 0x00, 0x01,
                                              0x00, 0x01, 0x00,
                                              0xC0, 0x01, 0x0A,
                                              0xC1, 0x02, 0x01, 0x00,
                                              0xC2, 0x02, 0x03, 0x02};

                s->connections++;
                if (ncl_socket_send(peer, kCc, sizeof(kCc)) != NCL_OK) {
                    break;
                }
                continue;
            }
            if (payload_len >= 3 && payload[1] == NCL_S7_COTP_DT) {
                size_t pdu_len = s7_slave_pdu(s, payload + 3, payload_len - 3, s7,
                                              sizeof(s7));
                size_t cotp_len = 7u + pdu_len;

                frame[0] = 0x03;
                frame[1] = 0x00;
                frame[2] = (uint8_t)(cotp_len >> 8);
                frame[3] = (uint8_t)cotp_len;
                frame[4] = 0x02;
                frame[5] = NCL_S7_COTP_DT;
                frame[6] = 0x80;
                memcpy(frame + 7, s7, pdu_len);
                if (ncl_socket_send(peer, frame, cotp_len) != NCL_OK) {
                    break;
                }
                continue;
            }
            break; /* something we do not speak */
        }
        ncl_socket_close(peer);
    }
}

static s7_slave *s7_slave_start(void)
{
    s7_slave *s = (s7_slave *)ncl_mem_calloc(1, sizeof(*s));
    size_t i;

    if (s == NULL) {
        return NULL;
    }
    s->pdu_size = 480;
    s->listener = ncl_socket_listen(0, NULL, 0);
    if (s->listener == NULL) {
        ncl_free_safe(s);
        return NULL;
    }
    s->port = ncl_socket_local_port(s->listener);
    for (i = 0; i < 64; i++) {
        s->flags[i] = (uint8_t)(i & 0xFF);
        s->db1[i] = 0;
    }
    s->thread = ncl_thread_start(s7_slave_main, s);
    if (s->thread == NULL) {
        ncl_socket_close(s->listener);
        ncl_free_safe(s);
        return NULL;
    }
    return s;
}

static void s7_slave_stop(s7_slave *s)
{
    if (s == NULL) {
        return;
    }
    s->stop = true;
    ncl_thread_join(s->thread);
    ncl_socket_close(s->listener);
    ncl_free_safe(s);
}

static ncl_driver *s7_driver(s7_slave *s, const char *extra)
{
    ncl_driver *driver = ncl_driver_create("s7_tcp");
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
    s7_slave *s = s7_slave_start();
    ncl_driver *driver;
    ncl_err err = NCL_ERR;
    long long number;

    NCL_TEST_CASE("the handshake is COTP connect, then Setup Communication");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = s7_driver(s, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        s7_slave_stop(s);
        return;
    }
    number = read_int(driver, "\"MB8\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 8);
    NCL_CHECK_EQ_INT(s->connections, 1);
    NCL_CHECK(s->requests >= 2); /* setup communication, then the read */

    NCL_TEST_CASE("a word of M is big endian");
    {
        ncl_address address;
        ncl_json *value = NULL;
        long long word = 0;

        s->flags[20] = 0x12;
        s->flags[21] = 0x34;
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"MW\",\"offset\":20}",
                                       &address),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_read_one(driver, &address, &value), NCL_OK);
        NCL_CHECK(ncl_json_as_int(value, &word));
        NCL_CHECK_EQ_INT(word, 0x1234);
        ncl_json_free(value);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("a real comes back from DB1");
    {
        ncl_address address;
        ncl_json *value = NULL;
        double real = 0;

        s->db1[0] = 0x3F;
        s->db1[1] = 0xC0;
        s->db1[2] = 0x00;
        s->db1[3] = 0x00;
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"DB1\",\"offset\":0,"
                                       "\"dtype\":\"float32\"}",
                                       &address),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_read_one(driver, &address, &value), NCL_OK);
        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
        ncl_json_free(value);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("writing M and reading it back");
    {
        ncl_address address;

        NCL_CHECK_EQ_INT(parse_address("\"M10.0\"", &address), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address,
                                              ncl_json_new_bool(true)),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->flags[10] & 0x01, 1);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("a batch is one Read Var carrying several items");
    {
        ncl_address addresses[3];
        ncl_json *values = NULL;
        int before = s->requests;

        NCL_CHECK_EQ_INT(parse_address("\"MB0\"", &addresses[0]), NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("\"MB1\"", &addresses[1]), NCL_OK);
        NCL_CHECK_EQ_INT(parse_address("{\"area\":\"DB1\",\"offset\":8,"
                                       "\"dtype\":\"int32\"}",
                                       &addresses[2]),
                         NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 3, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(s->requests - before, 1); /* one PDU for three points */
        {
            long long first = -1;
            long long second = -1;

            NCL_CHECK(ncl_json_as_int(ncl_json_arr_get(values, 0), &first));
            NCL_CHECK(ncl_json_as_int(ncl_json_arr_get(values, 1), &second));
            NCL_CHECK_EQ_INT(first, 0); /* MB0 was cleared by the read path */
            NCL_CHECK_EQ_INT(second, 1);
        }
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);
        ncl_address_clear(&addresses[2]);
    }

    NCL_TEST_CASE("a DB that is not there is the device's business error");
    {
        /* the mock PLC only knows DB1 */
        ncl_json *node = ncl_json_parse_cstr("{\"area\":\"DB7\",\"offset\":0}",
                                             NULL);
        ncl_address address;
        ncl_json *value = NULL;

        NCL_CHECK_EQ_INT(ncl_address_from_json(node, &address), NCL_OK);
        ncl_json_free(node);
        err = ncl_driver_read_one(driver, &address, &value);
        NCL_CHECK_EQ_INT(err, NCL_DRV_ERR_BUSINESS(0x30A));
        NCL_CHECK(value == NULL);
        ncl_address_clear(&address);
    }

    NCL_TEST_CASE("the session reopens on demand, handshake included");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    number = read_int(driver, "\"MB1\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 1);
    NCL_CHECK_EQ_INT(s->connections, 2);

    driver->ops->destroy(driver);
    s7_slave_stop(s);
}

static void test_through_the_manager(void)
{
    s7_slave *s = s7_slave_start();
    test_point_map *manager = test_point_map_create(ncl_s7_tcp_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured S7 link reads through the point map");
    NCL_CHECK(s != NULL && manager != NULL);
    if (s == NULL || manager == NULL) {
        s7_slave_stop(s);
        test_point_map_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"plc\",\"path\":\"/PLC\","
                            "\"type\":\"s7_tcp\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":600,\"slot\":1},"
                            "\"points\":["
                            "{\"path\":\"/PLC/BYTE\",\"addr\":\"MB5\"},"
                            "{\"path\":\"/PLC/REAL\",\"addr\":"
                            "{\"area\":\"DB1\",\"offset\":16,"
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

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/BYTE", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 5);
    ncl_json_free(value);
    value = NULL;

    NCL_CHECK_EQ_INT(test_point_map_write(manager, "/PLC/REAL",
                                              ncl_json_new_double(-2.5)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/REAL", &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == -2.5);
    }
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    s7_slave_stop(s);
}

static void test_silence(void)
{
    s7_slave *s = s7_slave_start();
    ncl_driver *driver;
    ncl_err err = NCL_ERR;

    NCL_TEST_CASE("a PLC that stops answering is a transport failure");
    NCL_CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    driver = s7_driver(s, NULL);
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        s->fails_to_skip = 1; /* the next frame gets no answer at all */
        (void)read_int(driver, "\"MB0\"", &err);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);
        driver->ops->destroy(driver);
    }
    s7_slave_stop(s);
}

NCL_TEST_MAIN_BEGIN()
    test_areas();
    test_frames();
    test_variables();
    test_against_a_plc();
    test_silence();
    test_through_the_manager();
NCL_TEST_MAIN_END()
