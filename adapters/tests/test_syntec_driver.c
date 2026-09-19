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
#include "nclink_adapter/ncl_driver_manager.h"
#include "nclink_adapter/ncl_syntec.h"
#include "syntec/ncl_syntec_driver.h"

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
} syntec_mock;

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
    ncl_driver *driver = ncl_driver_create("syntec");
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
    NCL_CHECK_EQ_INT(mock->last_code, 1); /* the point's offset is the dwCode */
    NCL_CHECK_EQ_INT(mock->last_size_out, 4);
    NCL_CHECK_EQ_INT(mock->last_serial, 1);

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

    driver->ops->destroy(driver);
    mock_stop(mock);
}

static void test_through_the_manager(void)
{
    syntec_mock *mock = mock_start();
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured SYNTEC link reads through the point map");
    NCL_CHECK(mock != NULL && manager != NULL);
    if (mock == NULL || manager == NULL) {
        mock_stop(mock);
        ncl_driver_manager_free(manager);
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
                            "\"dtype\":\"int32\"}}]}",
                            mock->port);
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

    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/CNC/PART", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 300);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(mock->last_code, 3);
    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/CNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    ncl_driver_manager_free(manager);
    mock_stop(mock);
}

NCL_TEST_MAIN_BEGIN()
    test_read();
    test_through_the_manager();
NCL_TEST_MAIN_END()
