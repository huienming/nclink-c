/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The SYNTEC driver against a mock OCAPIServer: the mock reads a packet the way
 * the real server does (12 byte header, then `Length` more), answers with the
 * same shape and echoes the request's uSerial.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/ncl_host.h"
#include "nclink/ncl_module.h"
#include "test_point_map.h"
#include "nclink/clients/syntec.h"
#include "syntec/ncl_syntec_driver.h"

#ifndef NCL_SYNTEC_PLUGIN_DIR
#  define NCL_SYNTEC_PLUGIN_DIR "" /* the adapter test is skipped without it */
#endif

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((in[1]) << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

/** The mock controller: it knows one command (200 = KrnlAPI) and the codes the
 *  test uses, which is exactly how a real one behaves for a known code. */
typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    int         requests;
    int         fails_to_skip;
    uint16_t    last_cmd;
    uint16_t    last_func;
    uint8_t     last_serial;
    int32_t     last_code;
    int32_t     last_size_out;
    uint8_t     answer[64];
    size_t      answer_len;
    bool        answer_as_is;
    /* §3.1/§3.2: the item service. The mock plays the controller: it answers
     * each item request with the request's 20 byte header echoed plus the
     * value, which is exactly what the probe found (tools/site-probe/
     * syntec_reply_probe.sh). */
    struct {
        uint32_t key;   /**< parameter B: the register / state number */
        uint16_t value; /**< the u16 it answers with                 */
    } items[16];
    size_t   item_count;
    char     program[64];      /**< the PROGRAM answer's text */
    bool     warning_body;     /**< WARNING with a body (never captured) */
    uint32_t item_log[8];      /**< parameter B of every item request, in order */
    size_t   item_log_count;
    uint16_t last_item_code;
    uint32_t last_item_b;
    /* 状态区：区号 -> 一串 int16（判 int16 + 10^-dec 缩放用） */
    struct {
        uint16_t zone;
        int16_t  values[8];
        size_t   count;
    } zones[4];
    size_t zone_count;
} syntec_mock;

/** The value the mock answers for one register / state number (0 when unset). */
static uint16_t mock_item_value(const syntec_mock *mock, uint32_t key)
{
    size_t i;

    for (i = 0; i < mock->item_count; i++) {
        if (mock->items[i].key == key) {
            return mock->items[i].value;
        }
    }
    return 0;
}

/** Script one answer: "when the request asks for @p key, answer @p value". */
static void mock_set_value(syntec_mock *mock, uint32_t key, uint16_t value)
{
    size_t i;

    for (i = 0; i < mock->item_count; i++) {
        if (mock->items[i].key == key) {
            mock->items[i].value = value;
            return;
        }
    }
    if (mock->item_count < sizeof(mock->items) / sizeof(mock->items[0])) {
        mock->items[mock->item_count].key = key;
        mock->items[mock->item_count].value = value;
        mock->item_count++;
    }
}

/** Script one state zone: "this zone answers these int16 values". */
static void mock_set_zone(syntec_mock *mock, uint16_t zone,
                          const int16_t *values, size_t count)
{
    size_t i;

    if (mock->zone_count >= sizeof(mock->zones) / sizeof(mock->zones[0]) ||
        count > 8) {
        return;
    }
    mock->zones[mock->zone_count].zone = zone;
    mock->zones[mock->zone_count].count = count;
    for (i = 0; i < count; i++) {
        mock->zones[mock->zone_count].values[i] = values[i];
    }
    mock->zone_count++;
}

static void mock_main(void *arg)
{
    syntec_mock *mock = (syntec_mock *)arg;

    while (!mock->stop) {
        ncl_socket *peer = ncl_socket_accept(mock->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t header[NCL_SYNTEC_PACKET_HEADER];
            uint8_t frame[256];
            uint8_t reply[256];
            ncl_syntec_view view;
            size_t content;
            size_t total;
            size_t reply_len;

            if (ncl_socket_recv_exact(peer, header, sizeof(header), 2000) != NCL_OK) {
                break;
            }
            content = get_u32(header);
            if (content < NCL_SYNTEC_FUNCTION_HEADER ||
                NCL_SYNTEC_PACKET_HEADER + content > sizeof(frame)) {
                break;
            }
            memcpy(frame, header, sizeof(header));
            if (ncl_socket_recv_exact(peer, frame + sizeof(header), content,
                                      2000) != NCL_OK) {
                break;
            }
            total = NCL_SYNTEC_PACKET_HEADER + content;
            if (mock->fails_to_skip > 0) {
                mock->fails_to_skip--;
                ncl_sleep_millis(200);
                break; /* no answer at all */
            }
            mock->requests++;
            if (ncl_syntec_split(frame, total, &view, NULL) != NCL_OK) {
                break;
            }
            mock->last_cmd = view.packet.cmd_id;
            mock->last_func = view.function.func_id;
            mock->last_serial = view.function.serial;
            if (view.packet.cmd_id == NCL_SYNTEC_CMD_ITEM &&
                view.body_len >= NCL_SYNTEC_ITEM_BODY) {
                /* §3.1: type | param A | param B | flag at [20..35]. */
                uint32_t param_b = get_u32(view.body + 8);
                uint32_t param_a = get_u32(view.body + 4);
                uint32_t request = get_u32(frame + 16);
                uint16_t code = get_u16(frame + 10);
                size_t item_body = 0;
                size_t z;
                bool zone_handled = false;
                bool send_failed = false;

                /* 状态区读（位置）：A = 4 + 2*count、B = 区号；答案是 count 个 int16。 */
                for (z = 0; z < mock->zone_count; z++) {
                    size_t want;
                    size_t n;
                    size_t k;

                    if (request != 0x0407u ||
                        mock->zones[z].zone != (uint16_t)param_b) {
                        continue;
                    }
                    want = param_a >= 4u ? (size_t)(param_a - 4u) / 2u : 0u;
                    n = want < mock->zones[z].count ? want : mock->zones[z].count;
                    memcpy(reply, frame, NCL_SYNTEC_REPLY_BODY);
                    for (k = 0; k < n; k++) {
                        reply[NCL_SYNTEC_REPLY_BODY + k * 2u] =
                            (uint8_t)((uint16_t)mock->zones[z].values[k] & 0xFF);
                        reply[NCL_SYNTEC_REPLY_BODY + k * 2u + 1u] =
                            (uint8_t)(((uint16_t)mock->zones[z].values[k] >> 8) & 0xFF);
                    }
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + n * 2u));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY + n * 2u) != NCL_OK) {
                        send_failed = true;
                    }
                    zone_handled = true;
                    break;
                }
                if (zone_handled) {
                    if (send_failed) {
                        break;
                    }
                    continue;
                }

                mock->last_item_b = param_b;
                mock->last_item_code = code;
                if (mock->item_log_count <
                    sizeof(mock->item_log) / sizeof(mock->item_log[0])) {
                    mock->item_log[mock->item_log_count++] = param_b;
                }
                /* The answer repeats the request's 20 byte header and carries
                 * the value after it (§3.2). */
                memcpy(reply, frame, NCL_SYNTEC_REPLY_BODY);
                if (code == 0x071eu) {          /* PROGRAM: the body is text  */
                    item_body = strlen(mock->program);
                    if (item_body > 0) {
                        memcpy(reply + NCL_SYNTEC_REPLY_BODY, mock->program,
                               item_body);
                    }
                } else if (code == 0x0701u) {   /* WARNING: usually no body   */
                    if (mock->warning_body) {
                        reply[NCL_SYNTEC_REPLY_BODY] = 0x07;
                        reply[NCL_SYNTEC_REPLY_BODY + 1] = 0x00;
                        item_body = 2;
                    }
                } else {                        /* a u16 at [20..21]          */
                    uint16_t value = mock_item_value(mock, param_b);
                    reply[NCL_SYNTEC_REPLY_BODY] = (uint8_t)(value & 0xFF);
                    reply[NCL_SYNTEC_REPLY_BODY + 1] = (uint8_t)(value >> 8);
                    item_body = 2;
                }
                /* Length counts what follows the 12 byte header. */
                put_u32(reply,
                        (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + item_body));
                if (ncl_socket_send(peer, reply,
                                    NCL_SYNTEC_REPLY_BODY + item_body) != NCL_OK) {
                    break;
                }
                continue;
            }
            /* The body is MMI_Request_KrnlAPI: funcID u2, code i4, sizeIn i4,
             * sizeOut i4, then the input bytes (§10.2). */
            if (view.body_len >= 14) {
                mock->last_code = (int32_t)get_u32(view.body + 2);
                mock->last_size_out = (int32_t)get_u32(view.body + 10);
            }
            {
                uint8_t body[160];
                size_t body_len = 0;
                ncl_syntec_function function;

                memset(&function, 0, sizeof(function));
                function.func_id = view.function.func_id;
                function.serial = view.function.serial; /* echo it back */
                put_u32(body + body_len, (uint32_t)view.function.func_id);
                body_len += 2;
                put_u32(body + body_len, (uint32_t)mock->last_code);
                body_len += 4;
                put_u32(body + body_len, 0);
                body_len += 4;
                put_u32(body + body_len, (uint32_t)mock->answer_len);
                body_len += 4;
                memcpy(body + body_len, mock->answer, mock->answer_len);
                body_len += mock->answer_len;
                reply_len = ncl_syntec_build(reply, sizeof(reply),
                                             view.packet.cmd_id, &function, body,
                                             body_len);
            }
            if (reply_len == 0 || ncl_socket_send(peer, reply, reply_len) != NCL_OK) {
                break;
            }
            (void)mock->answer_as_is;
        }
        ncl_socket_close(peer);
    }
}

static syntec_mock *mock_start(void)
{
    syntec_mock *mock = (syntec_mock *)ncl_mem_calloc(1, sizeof(*mock));

    if (mock == NULL) {
        return NULL;
    }
    mock->listener = ncl_socket_listen(0, NULL, 0);
    if (mock->listener == NULL) {
        ncl_free_safe(mock);
        return NULL;
    }
    mock->port = ncl_socket_local_port(mock->listener);
    mock->thread = ncl_thread_start(mock_main, mock);
    if (mock->thread == NULL) {
        ncl_socket_close(mock->listener);
        ncl_free_safe(mock);
        return NULL;
    }
    return mock;
}

static void mock_stop(syntec_mock *mock)
{
    if (mock == NULL) {
        return;
    }
    mock->stop = true;
    ncl_thread_join(mock->thread);
    ncl_socket_close(mock->listener);
    ncl_free_safe(mock);
}

static ncl_driver *syntec_driver(syntec_mock *mock, const char *extra)
{
    ncl_driver *driver = ncl_syntec_create();
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":800,\"retries\":0%s}",
                            mock->port, extra != NULL ? extra : "");
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

static ncl_err read_point(ncl_driver *driver, const char *area, long long code,
                          int length, const char *dtype, ncl_json **value)
{
    ncl_strbuf json;
    ncl_json *node;
    ncl_address address;
    ncl_err err;

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"area\":\"%s\",\"offset\":%lld,\"length\":%d,"
                            "\"dtype\":\"%s\"}",
                            area, code, length, dtype);
    node = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, &address);
    ncl_json_free(node);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_read_one(driver, &address, value);
    ncl_address_clear(&address);
    return err;
}

static void test_read(void)
{
    syntec_mock *mock = mock_start();
    ncl_driver *driver;
    ncl_json *value = NULL;
    ncl_err err;

    NCL_TEST_CASE("a KrnlAPI read goes out as CmdID 200 and the code the point says");
    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    /* The mock answers 0x0000002A (42) as an int32. */
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x00;
    mock->answer[3] = 0x2A;
    mock->answer_len = 4;
    driver = syntec_driver(mock, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mock_stop(mock);
        return;
    }
    NCL_CHECK_EQ_INT(read_point(driver, "KrnlAPI", 1, 4, "int32", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 42);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->last_cmd, NCL_SYNTEC_CMD_KRML_API);
    /* §10.9: the function header carries the same number as the packet's CmdID */
    NCL_CHECK_EQ_INT(mock->last_func, NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(mock->last_code, 1); /* the point's offset is the dwCode */
    NCL_CHECK_EQ_INT(mock->last_size_out, 4);
    NCL_CHECK_EQ_INT(mock->last_serial, 1);

    NCL_TEST_CASE("§6: the driver hands the frames of the exchange to the audit");
    {
        ncl_driver_raw raw;
        uint32_t length;

        ncl_driver_last_raw(driver, &raw);
        /* 12 byte packet header + 8 byte function header + the 14 byte body */
        NCL_CHECK_EQ_INT(raw.request_len, 34);
        NCL_CHECK(raw.request != NULL);
        if (raw.request != NULL && raw.request_len == 34) {
            length = get_u32(raw.request);
            NCL_CHECK_EQ_INT(length, 22); /* §10.3: everything after the header */
            NCL_CHECK_EQ_INT(get_u16(raw.request + 4), NCL_SYNTEC_CMD_KRML_API);
            NCL_CHECK_EQ_INT(get_u16(raw.request + 12), NCL_SYNTEC_CMD_KRML_API);
            NCL_CHECK_EQ_INT(raw.request[14], 1); /* the serial that went out */
            /* §10.9 again: the body's uFuncID repeats the command number */
            NCL_CHECK_EQ_INT(get_u16(raw.request + 20), NCL_SYNTEC_CMD_KRML_API);
            NCL_CHECK_EQ_INT(get_u32(raw.request + 22), 1); /* dwCode */
            NCL_CHECK_EQ_INT(get_u32(raw.request + 30), 4); /* dwSizeOut */
        }
        NCL_CHECK(raw.reply != NULL);
        NCL_CHECK(raw.reply_len >= NCL_SYNTEC_PACKET_HEADER);
        if (raw.reply != NULL && raw.reply_len >= NCL_SYNTEC_PACKET_HEADER) {
            NCL_CHECK_EQ_INT(get_u16(raw.reply + 4), NCL_SYNTEC_CMD_KRML_API);
        }
    }

    NCL_TEST_CASE("the serial advances, and a bare command number is accepted");
    /* 1.5 as a big endian double, which is what this point asks for */
    mock->answer[0] = 0x3F;
    mock->answer[1] = 0xF8;
    memset(mock->answer + 2, 0, 6);
    mock->answer_len = 8;
    NCL_CHECK_EQ_INT(read_point(driver, "200", 5, 8, "float64", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->last_serial, 2);
    NCL_CHECK_EQ_INT(mock->last_code, 5);

    NCL_TEST_CASE("an unknown command name is a point map error");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(read_point(driver, "NoSuch", 1, 4,
                                                     "int32", &value)),
                     3);

    NCL_TEST_CASE("writing is refused: the box lists no SYNTEC write endpoint");
    NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, NULL, NULL),
                     NCL_ERR_NOT_SUPPORTED);

    NCL_TEST_CASE("the session reports itself");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "serial", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "serial", -1), 2);
        ncl_json_free(result);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("the raw hatch takes CmdID + funcId + body");
    {
        ncl_driver_result out;
        /* CmdID 200, funcId 1, then the four KrnlAPI fields */
        const uint8_t raw[] = {0xC8, 0x00, 0x01, 0x00,
                               0x01, 0x00, 0x07, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00,
                               0x04, 0x00, 0x00, 0x00};

        mock->answer_len = 4; /* the answer the raw reply should carry */
        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, raw, sizeof(raw), &out),
                         NCL_OK);
        NCL_CHECK(out.success);
        NCL_CHECK_EQ_INT(out.raw_len, 18); /* 14 fields + the 4 answer bytes */
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("a controller that stops answering is a transport failure");
    mock->fails_to_skip = 1;
    err = read_point(driver, "KrnlAPI", 1, 4, "int32", &value);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);

    NCL_TEST_CASE("closing and reopening works");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    NCL_CHECK_EQ_INT(read_point(driver, "KrnlAPI", 1, 4, "int32", &value), NCL_OK);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK(driver->ops->is_connected(driver));

    NCL_TEST_CASE("§10.12: a named reading asks KrnlAPI for its own code");
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x03;
    mock->answer[3] = 0xE8; /* 1000 parts */
    mock->answer_len = 4;
    /* The client writes it "READ_part_count"; the point map is lenient about it,
     * and a reading carries its own code, so the offset is not used. */
    NCL_CHECK_EQ_INT(read_point(driver, "READ_part_count", 7, 4, "int32", &value),
                     NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 1000);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->last_cmd, NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(mock->last_code, 1000); /* from the table, not the offset 7 */
    NCL_CHECK_EQ_INT(mock->last_size_out, 4);

    driver->ops->destroy(driver);
    mock_stop(mock);
}

static void test_through_the_manager(void)
{
    syntec_mock *mock = mock_start();
    test_point_map *manager = test_point_map_create(ncl_syntec_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured SYNTEC link reads through the point map");
    NCL_CHECK(mock != NULL && manager != NULL);
    if (mock == NULL || manager == NULL) {
        mock_stop(mock);
        test_point_map_free(manager);
        return;
    }
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x01;
    mock->answer[3] = 0x2C; /* 300 */
    mock->answer_len = 4;
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"cnc\",\"path\":\"/CNC\","
                            "\"type\":\"syntec\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":800,\"funcId\":1},"
                            "\"points\":["
                            "{\"path\":\"/CNC/PART\",\"addr\":"
                            "{\"area\":\"KrnlAPI\",\"offset\":3,\"length\":4,"
                            "\"dtype\":\"int32\"}},"
                            "{\"path\":\"/CNC/COUNT\",\"addr\":"
                            "{\"area\":\"part_count\",\"length\":4,"
                            "\"dtype\":\"int32\"}}]}",
                            mock->port);
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

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/PART", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 300);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(mock->last_code, 3);

    NCL_TEST_CASE("a point may name a §10.12 reading instead of its code");
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x00;
    mock->answer[3] = 0xFA; /* 250 */
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/COUNT", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 250);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(mock->last_cmd, NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(mock->last_code, 1000); /* the total part counter */

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    mock_stop(mock);
}

/*
 * §3.1/§3.2: the nine items the delivery closed the loop on. The item table is
 * checked against the captured STATUS frame byte for byte, and the nine getters
 * run against the mock controller: it answers every item request the way the
 * probe found a real one does - request echoed, value after the 20 byte header.
 */
static void test_items(void)
{
    /* The full STATUS frame from the doc (§3.1), with uSerial 0. */
    static const uint8_t kStatusFrame[NCL_SYNTEC_ITEM_FRAME] = {
        0x18, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x07,
        0xc8, 0x00, 0x00, 0x00, 0x07, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    uint8_t frame[NCL_SYNTEC_ITEM_FRAME];
    const ncl_syntec_item *item;
    syntec_mock *mock;
    ncl_syntec_config config;
    ncl_syntec *session;
    char text[64];
    char *err = NULL;
    long long number = 0;
    double speed = 0.0;
    ncl_json *warnings = NULL;

    NCL_TEST_CASE("§3.1: the item table holds the nine closed loop items");
    NCL_CHECK_EQ_INT(ncl_syntec_item_count(), 9);
    NCL_CHECK(ncl_syntec_item_lookup("STATUS") != NULL);
    NCL_CHECK(ncl_syntec_item_lookup("read_status") != NULL);
    item = ncl_syntec_item_lookup("partCount");
    NCL_CHECK(item != NULL && item->param_b == 1000u);
    NCL_CHECK(ncl_syntec_item_lookup("no-such-item") == NULL);
    NCL_CHECK(ncl_syntec_item_at(ncl_syntec_item_count()) == NULL);

    NCL_TEST_CASE("§3.1: the STATUS request is the captured 36 bytes");
    item = ncl_syntec_item_lookup("STATUS");
    NCL_CHECK(item != NULL);
    NCL_CHECK_EQ_INT(
        ncl_syntec_item_frame(frame, sizeof(frame), item, item->param_b, 0),
        NCL_SYNTEC_ITEM_FRAME);
    NCL_CHECK(memcmp(frame, kStatusFrame, sizeof(kStatusFrame)) == 0);

    NCL_TEST_CASE("§3.1: param B is the register, uSerial the only moving byte");
    item = ncl_syntec_item_lookup("FEED_SPEED");
    NCL_CHECK(item != NULL);
    NCL_CHECK_EQ_INT(
        ncl_syntec_item_frame(frame, sizeof(frame), item, 12, 7),
        NCL_SYNTEC_ITEM_FRAME);
    NCL_CHECK_EQ_INT(get_u16(frame + 4), NCL_SYNTEC_CMD_ITEM);
    NCL_CHECK_EQ_INT(get_u16(frame + 12), NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(get_u32(frame + 28), 12u);
    NCL_CHECK_EQ_INT(frame[14], 7);
    NCL_CHECK_EQ_INT(frame[6], 0); /* FEED_SPEED's own flags */

    mock = mock_start();
    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    mock_set_value(mock, 1000u, 1234u); /* PART_COUNT */
    mock_set_value(mock, 10u, 4321u);   /* LINE_NUMBER */
    mock_set_value(mock, 19u, 80u);     /* FEED_OVERRIDE */
    mock_set_value(mock, 771u, 9000u);  /* SPDL_SPEED */
    mock_set_value(mock, 21u, 90u);     /* SPDL_OVERRIDE */
    mock_set_value(mock, 700u, 4321u);  /* FEED_SPEED's register */
    mock_set_value(mock, 12u, 0u);      /* its unit state */
    mock_set_value(mock, 76u, 70u);     /* its mode state */
    snprintf(mock->program, sizeof(mock->program), "O1000");

    ncl_syntec_config_default(&config);
    NCL_CHECK_EQ_INT(config.port, 8000);
    config.host = "127.0.0.1";
    config.port = mock->port;
    config.timeout_ms = 800;
    NCL_CHECK(ncl_syntec_open(&config, &err) != NULL || err != NULL);
    session = ncl_syntec_open(&config, &err);
    NCL_CHECK(session != NULL);
    if (session == NULL) {
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("§3.2: STATUS maps the state enum");
    mock_set_value(mock, 4u, 2u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "running");
    mock_set_value(mock, 4u, 3u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "holding");
    mock_set_value(mock, 4u, 4u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "free");
    mock_set_value(mock, 4u, 9u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "unknown");

    NCL_TEST_CASE("§3.2: the numeric items read their u16");
    NCL_CHECK_EQ_INT(ncl_syntec_part_count(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 1234);
    NCL_CHECK_EQ_INT(mock->last_item_b, 1000u);
    NCL_CHECK_EQ_INT(ncl_syntec_line_number(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 4321);
    NCL_CHECK_EQ_INT(ncl_syntec_feed_override(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 80);
    NCL_CHECK_EQ_INT(ncl_syntec_spindle_speed(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 9000);
    NCL_CHECK_EQ_INT(ncl_syntec_spindle_override(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 90);

    NCL_TEST_CASE("§3.2: PROGRAM comes back as the body's text");
    NCL_CHECK_EQ_INT(ncl_syntec_program(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "O1000");

    NCL_TEST_CASE("§3.2: FEED_SPEED asks 700, then the states 12 and 76");
    mock->item_log_count = 0;
    NCL_CHECK_EQ_INT(ncl_syntec_feed_speed(session, &speed), NCL_OK);
    NCL_CHECK_EQ_INT((long long)speed, 4321);
    NCL_CHECK_EQ_INT(mock->item_log_count, 3);
    NCL_CHECK_EQ_INT(mock->item_log[0], 700u);
    NCL_CHECK_EQ_INT(mock->item_log[1], 12u);
    NCL_CHECK_EQ_INT(mock->item_log[2], 76u);

    NCL_TEST_CASE("§3.2: a unit state of (0,0) is the factor 1.0");
    mock_set_value(mock, 76u, 1u);
    NCL_CHECK_EQ_INT(ncl_syntec_feed_speed(session, &speed), NCL_OK);
    NCL_CHECK_EQ_INT((long long)speed, 4321);

    NCL_TEST_CASE("§3.2: an uncaptured unit step says 还读不了, not a guess");
    mock_set_value(mock, 12u, 32u);
    NCL_CHECK_EQ_INT(ncl_syntec_feed_speed(session, &speed),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "单位换算表") != NULL);

    NCL_TEST_CASE("§3.2: WARNING with no body is an empty list");
    NCL_CHECK_EQ_INT(ncl_syntec_warning(session, &warnings), NCL_OK);
    NCL_CHECK(warnings != NULL);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(warnings), 0);
    ncl_json_free(warnings);
    warnings = NULL;

    NCL_TEST_CASE("§3.2: a populated WARNING was never captured");
    mock->warning_body = true;
    NCL_CHECK_EQ_INT(ncl_syntec_warning(session, &warnings),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "待抓包") != NULL);
    mock->warning_body = false;

    ncl_syntec_close(session);
    mock_stop(mock);
}

/** Index of a point by its model path, or (size_t)-1. */
static size_t host_point_index(const ncl_host *host, const char *path)
{
    size_t i;

    for (i = 0; i < ncl_host_point_count(host); i++) {
        const char *candidate = ncl_host_point_path(host, i);

        if (candidate != NULL && strcmp(candidate, path) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

/*
 * The adapter itself: plugins/syntec.c is loaded as a module by protocol name
 * (exactly the path the device program takes), the host turns its declaration
 * into the model, and the nine points are read over the same mock controller.
 * That is the "整机仿真" 10 册 §3.2 promised: adapter + client + captured frames.
 */
static void test_adapter(void)
{
    static const char *kPaths[] = {
        "/MACHINE/STATUS",           "/MACHINE/PART_COUNT",
        "/MACHINE/CONTROLLER/PROGRAM", "/MACHINE/CONTROLLER/WARNING",
        "/MACHINE/CONTROLLER/LINE_NUMBER", "/MACHINE/FEED_OVERRIDE",
        "/MACHINE/SPINDLE_OVERRIDE", "/MACHINE/FEED_SPEED",
        "/MACHINE/SPINDLE_SPEED",
        "/MACHINE/AXIS@X/MOTOR/POSITION", "/MACHINE/AXIS@Z/MOTOR/POSITION",
        "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@Z/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@X/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@Z/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@X/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@Z/MOTOR/VARIABLE@DISTANCE"};
    syntec_mock *mock;
    ncl_module_set *modules;
    ncl_strbuf err;
    ncl_strbuf json;
    ncl_json *config;
    ncl_host *host = NULL;
    const ncl_json *value;
    long long number = 0;
    double real = 0.0;
    size_t i;

    if (NCL_SYNTEC_PLUGIN_DIR[0] == '\0') {
        return; /* built without the adapter modules (NCLINK_BUILD_PLUGINS=OFF) */
    }
    mock = mock_start();
    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    mock_set_value(mock, 4u, 2u);      /* STATUS = running */
    mock_set_value(mock, 1000u, 1234u);
    mock_set_value(mock, 10u, 4321u);
    mock_set_value(mock, 19u, 80u);
    mock_set_value(mock, 771u, 9000u);
    mock_set_value(mock, 21u, 90u);
    mock_set_value(mock, 700u, 4321u);
    mock_set_value(mock, 12u, 0u);
    mock_set_value(mock, 76u, 70u);
    snprintf(mock->program, sizeof(mock->program), "O1000");

    modules = ncl_modules_create();
    ncl_strbuf_init(&err);
    NCL_CHECK(modules != NULL);
    NCL_TEST_CASE("the syntec adapter loads as a tool module");
    if (modules == NULL ||
        ncl_modules_add(modules, "syntec", NCL_SYNTEC_PLUGIN_DIR, &err) != NCL_OK) {
        NCL_CHECK_EQ_INT((int)err.len, 0);
        ncl_strbuf_free(&err);
        mock_stop(mock);
        return;
    }
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK_EQ_STR(ncl_module_name(modules, 0), "syntec");

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(
        &json,
        "{ \"sn\": \"V000000001\","
        "  \"tools\": [ { \"name\": \"syntec\", \"parameters\": {"
        "     \"host\": \"127.0.0.1\", \"port\": %u, \"timeoutMs\": 800 } } ],"
        "  \"device\": { \"type\": \"MACHINE\", \"id\": \"01\","
        "                \"name\": \"新代机床\" },"
        "  \"sample\": { \"intervalMs\": 250, \"uploadMs\": 250 } }",
        mock->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), &err);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    if (config != NULL) {
        host = ncl_host_create_with_modules(config, modules, &err);
        ncl_json_free(config);
    }
    NCL_CHECK(host != NULL);
    if (host == NULL) {
        ncl_strbuf_free(&err);
        ncl_modules_free(modules);
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("the nine points are the model the device publishes");
    NCL_CHECK_EQ_INT(ncl_host_point_count(host), 17);
    for (i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); i++) {
        NCL_CHECK(host_point_index(host, kPaths[i]) != (size_t)-1);
    }
    /* 模型里就是这台机床的能力面：路径是按树推出来的（模型文档里没有 path 字段），
     * 采样通道只引用四样（状态、计件、程序名、报警）。 */
    {
        ncl_node *part = ncl_node_find_by_id(ncl_server_model(ncl_host_server(host)),
                                            "p1");
        NCL_CHECK(part != NULL);
        if (part != NULL) {
            NCL_CHECK_EQ_STR(ncl_node_path(part), "/MACHINE/PART_COUNT");
        }
        NCL_CHECK(ncl_node_find_by_type(ncl_server_model(ncl_host_server(host)),
                                        NCL_NODE_TYPE_SAMPLE_CHANNEL) != NULL);
    }

    NCL_TEST_CASE("the adapter reads the nine items off the mock controller");
    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/STATUS", &err), NCL_OK);
    value = ncl_host_point_value(host, host_point_index(host, "/MACHINE/STATUS"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "running");

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/PART_COUNT", &err), NCL_OK);
    value = ncl_host_point_value(host,
                                 host_point_index(host, "/MACHINE/PART_COUNT"));
    NCL_CHECK(value != NULL && ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 1234);

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/CONTROLLER/PROGRAM", &err),
                     NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/CONTROLLER/PROGRAM"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "O1000");

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/CONTROLLER/WARNING", &err),
                     NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/CONTROLLER/WARNING"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(value), 0); /* no alarm = empty list */

    NCL_CHECK_EQ_INT(
        ncl_host_poll_one(host, "/MACHINE/CONTROLLER/LINE_NUMBER", &err), NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/CONTROLLER/LINE_NUMBER"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "4321"); /* 表 7 是 string */

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/FEED_OVERRIDE", &err),
                     NCL_OK);
    value = ncl_host_point_value(host,
                                 host_point_index(host, "/MACHINE/FEED_OVERRIDE"));
    NCL_CHECK(value != NULL && ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 80);

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/SPINDLE_OVERRIDE", &err),
                     NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/SPINDLE_OVERRIDE"));
    NCL_CHECK(value != NULL && ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 90);

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/SPINDLE_SPEED", &err),
                     NCL_OK);
    value = ncl_host_point_value(host,
                                 host_point_index(host, "/MACHINE/SPINDLE_SPEED"));
    NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
    NCL_CHECK_EQ_INT((long long)real, 9000); /* 主轴转速，rpm */

    NCL_TEST_CASE("FEED_SPEED goes through its three frames here too");
    mock->item_log_count = 0;
    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/FEED_SPEED", &err), NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/FEED_SPEED"));
    NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
    NCL_CHECK_EQ_INT((long long)real, 4321);
    NCL_CHECK_EQ_INT(mock->item_log_count, 3);

    NCL_TEST_CASE("位置走状态区：int16 + 10^-小数位");
    {
        /* 房把 X 摆到 1.234、Z 摆到 -0.5（int16 原值），小数位 3。 */
        static const int16_t kMachine[2] = {1234, -500};
        static const int16_t kDecimals[1] = {3};
        static const int16_t kAbsolute[2] = {2000, -1};

        mock_set_zone(mock, NCL_SYNTEC_ZONE_MACHINE, kMachine, 2);
        mock_set_zone(mock, NCL_SYNTEC_ZONE_DECIMALS, kDecimals, 1);
        mock_set_zone(mock, NCL_SYNTEC_ZONE_ABSOLUTE, kAbsolute, 2);

        value = NULL;
        NCL_CHECK_EQ_INT(
            ncl_host_poll_one(host, "/MACHINE/AXIS@X/MOTOR/POSITION", &err),
            NCL_OK);
        value = ncl_host_point_value(
            host, host_point_index(host, "/MACHINE/AXIS@X/MOTOR/POSITION"));
        NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
        NCL_CHECK_EQ_INT((long long)(real * 1000.0 + 0.5), 1234);

        NCL_CHECK_EQ_INT(
            ncl_host_poll_one(host, "/MACHINE/AXIS@Z/MOTOR/POSITION", &err),
            NCL_OK);
        value = ncl_host_point_value(
            host, host_point_index(host, "/MACHINE/AXIS@Z/MOTOR/POSITION"));
        NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
        NCL_CHECK_EQ_INT((long long)(real * 1000.0 - 0.5), (long long)-500);

        NCL_CHECK_EQ_INT(ncl_host_poll_one(
                             host, "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE", &err),
                         NCL_OK);
        value = ncl_host_point_value(
            host,
            host_point_index(host, "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE"));
        NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
        NCL_CHECK_EQ_INT((long long)(real * 1000.0 + 0.5), 2000);
    }

    ncl_host_free(host);
    ncl_strbuf_free(&err);
    ncl_modules_free(modules);
    mock_stop(mock);
}

NCL_TEST_MAIN_BEGIN()
    test_read();
    test_through_the_manager();
    test_items();
    test_adapter();
NCL_TEST_MAIN_END()
