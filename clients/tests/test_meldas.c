/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Mitsubishi M70/M80 (MELDAS/GIOP): the 80 byte request of 05-MITSUBISHI-CNC-
 * M70-MELDAS.md §6.1 byte for byte, then a mock machine the test runs itself.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "test_point_map.h"
#include "nclink/clients/meldas.h"
#include "meldas/ncl_meldas_driver.h"

static bool binary_equal(const uint8_t *actual, const char *expected, size_t len)
{
    return memcmp(actual, expected, len) == 0;
}

/* ============================================================ the bytes == */

static void test_request(void)
{
    ncl_meldas_request fields;
    uint8_t frame[128];
    size_t len;

    NCL_TEST_CASE("the request of §6.1 is reproduced byte for byte");
    memset(&fields, 0, sizeof(fields));
    fields.command = 0x25; /* read coordinates */
    fields.subcode = 2;    /* machine         */
    fields.count = 1;
    fields.address = 1; /* X */
    fields.want = NCL_MELDAS_TYPE_STRING;
    len = ncl_meldas_build_request(frame, sizeof(frame), 0x11223344u,
                                   NCL_MELDAS_GET_DATA, &fields);
    NCL_CHECK_EQ_INT(len, 80);
    NCL_CHECK(binary_equal(frame,
                           "\x47\x49\x4f\x50\x01\x00\x01\x00"
                           "\x44\x00\x00\x00"
                           "\x00\x00\x00\x00"
                           "\x44\x33\x22\x11"
                           "\x01\xff\xff\xff"
                           "\x04\x00\x00\x00"
                           "\x01\x00\x00\x00"
                           "\x0d\x00\x00\x00"
                           "mochaGetData"
                           /* the NUL that ends the name is the first byte of
                            * the fixed 00 00 00 03 that follows it (§3.1) */
                           "\x00\x00\x00\x03"
                           "\x00\x00\x00\x00"
                           "\x25\x00\x00\x00"
                           "\x02\x00\x00\x00"
                           "\x01\x00\x00\x00"
                           "\x01\x00\x00\x00"
                           "\x00\x00\x00\x00"
                           "\x10\x00\x00\x00",
                           80));

    NCL_TEST_CASE("the length field is total - 12 (§8.1)");
    NCL_CHECK_EQ_INT(frame[8], 0x44);

    NCL_TEST_CASE("an operation name that does not fit is refused");
    NCL_CHECK_EQ_INT(ncl_meldas_build_request(frame, sizeof(frame), 1,
                                              "mochaTooLongName", &fields),
                     0);
    NCL_CHECK_EQ_INT(ncl_meldas_build_request(frame, 16, 1, NCL_MELDAS_GET_DATA,
                                              &fields),
                     0);
}

static void test_reply(void)
{
    ncl_meldas_value value;
    char err[160];

    NCL_TEST_CASE("a CString reply keeps its length at [36] and text at [40]");
    {
        uint8_t reply[48];
        uint32_t id = 0x11223344u;

        memset(reply, 0, sizeof(reply));
        memcpy(reply, "GIOP", 4);
        reply[4] = 0x01;
        reply[6] = 0x01;
        reply[7] = 0x01; /* Reply */
        reply[8] = 36;   /* body length */
        reply[16] = 0x44;
        reply[17] = 0x33;
        reply[18] = 0x22;
        reply[19] = 0x11;
        reply[28] = NCL_MELDAS_TYPE_STRING;
        reply[36] = 7; /* the length of "123.456" */
        memcpy(reply + 40, "123.456", 7);

        err[0] = '\0';
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), id, &value,
                                                err, sizeof(err)),
                         NCL_OK);
        NCL_CHECK(value.present);
        NCL_CHECK_EQ_INT(value.type, NCL_MELDAS_TYPE_STRING);
        NCL_CHECK_EQ_STR(value.text, "123.456");

        NCL_TEST_CASE("a reply to another request is refused");
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), id + 1,
                                                &value, err, sizeof(err)),
                         NCL_DRV_ERR_PROTOCOL(0x52));
        NCL_CHECK(strstr(err, "ID") != NULL);

        NCL_TEST_CASE("a frame that is still arriving is not an error");
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, 20, id, &value, err,
                                                sizeof(err)),
                         NCL_DRV_ERR_PROTOCOL(0x50));
        reply[1] = 'X';
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), id, &value,
                                                err, sizeof(err)),
                         NCL_DRV_ERR_PROTOCOL(0x51));
        reply[1] = 'I';
    }

    NCL_TEST_CASE("the IDL answer means there is no data (§3.2)");
    {
        uint8_t reply[40];

        memset(reply, 0, sizeof(reply));
        memcpy(reply, "GIOP", 4);
        reply[4] = 0x01;
        reply[6] = 0x01;
        reply[7] = 0x01;
        reply[8] = 28;
        memcpy(reply + 28, "IDL", 3);
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), 0, &value,
                                                NULL, 0),
                         NCL_OK);
        NCL_CHECK(!value.present);
        {
            ncl_json *json = ncl_meldas_value_to_json(&value, NCL_DTYPE_INT32);

            NCL_CHECK_EQ_INT(ncl_json_type_of(json), NCL_JSON_NULL);
            ncl_json_free(json);
        }
    }

    NCL_TEST_CASE("numbers are little endian");
    {
        uint8_t reply[48];
        ncl_json *json;

        memset(reply, 0, sizeof(reply));
        memcpy(reply, "GIOP", 4);
        reply[4] = 0x01;
        reply[6] = 0x01;
        reply[7] = 0x01;
        reply[8] = 36;
        reply[28] = NCL_MELDAS_TYPE_INT32;
        reply[36] = 0x2A; /* 42 */
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), 0, &value,
                                                NULL, 0),
                         NCL_OK);
        NCL_CHECK_EQ_INT(value.integer, 42);
        json = ncl_meldas_value_to_json(&value, NCL_DTYPE_INT32);
        NCL_CHECK_EQ_INT(ncl_json_type_of(json), NCL_JSON_NUMBER);
        ncl_json_free(json);

        reply[28] = NCL_MELDAS_TYPE_INT16;
        reply[36] = 0xFE;
        reply[37] = 0xFF; /* -2 */
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), 0, &value,
                                                NULL, 0),
                         NCL_OK);
        NCL_CHECK_EQ_INT(value.integer, -2);

        reply[28] = NCL_MELDAS_TYPE_DOUBLE;
        reply[36] = 0x00;
        reply[37] = 0x00;
        reply[38] = 0x00;
        reply[39] = 0x00;
        reply[40] = 0x00;
        reply[41] = 0x00;
        reply[42] = 0xF8;
        reply[43] = 0x3F; /* 1.5 */
        NCL_CHECK_EQ_INT(ncl_meldas_parse_reply(reply, sizeof(reply), 0, &value,
                                                NULL, 0),
                         NCL_OK);
        NCL_CHECK(value.real == 1.5);
    }

    NCL_TEST_CASE("the 10 byte double is read as x87 extended precision");
    {
        static const uint8_t kOneAndAHalf[10] = {0x00, 0x00, 0x00, 0x00,
                                                 0x00, 0x00, 0x00, 0xC0,
                                                 0xFF, 0x3F};

        NCL_CHECK_EQ_INT((long long)ncl_meldas_double10(kOneAndAHalf), 1);
        NCL_CHECK(ncl_meldas_double10(kOneAndAHalf) == 1.5);
    }
}

static void test_names(void)
{
    uint32_t command = 0;
    uint32_t subcode = 0;
    uint32_t want = 0;
    const char *canonical = NULL;

    NCL_TEST_CASE("the named commands of §4");
    NCL_CHECK(ncl_meldas_command_lookup("machine_position", &command, &subcode,
                                        &want, &canonical));
    NCL_CHECK_EQ_INT(command, 0x25);
    NCL_CHECK_EQ_INT(subcode, 2);
    NCL_CHECK_EQ_INT(want, NCL_MELDAS_TYPE_STRING);
    NCL_CHECK(ncl_meldas_command_lookup("SPINDLE_LOAD", &command, &subcode, &want,
                                        NULL));
    NCL_CHECK_EQ_INT(command, 0x3f);
    NCL_CHECK_EQ_INT(subcode, 4);
    NCL_CHECK(ncl_meldas_command_lookup("part_count", &command, &subcode, &want,
                                        NULL));
    NCL_CHECK_EQ_INT(command, 0x7e);
    NCL_CHECK_EQ_INT(subcode, 8002);
    NCL_CHECK(!ncl_meldas_command_lookup("no_such_thing", &command, &subcode,
                                         &want, NULL));

    NCL_TEST_CASE("the raw form names a command the table does not");
    NCL_CHECK(ncl_meldas_command_lookup("0x3b/0x7f", &command, &subcode, &want,
                                        NULL));
    NCL_CHECK_EQ_INT(command, 0x3b);
    NCL_CHECK_EQ_INT(subcode, 0x7f);
    NCL_CHECK(ncl_meldas_command_lookup("0x12", &command, &subcode, NULL, NULL));
    NCL_CHECK_EQ_INT(command, 0x12);
    NCL_CHECK_EQ_INT(subcode, 0);

    NCL_TEST_CASE("the axis coding of §5, both ways");
    NCL_CHECK_EQ_INT(ncl_meldas_axis(1, true), 1); /* X */
    NCL_CHECK_EQ_INT(ncl_meldas_axis(2, true), 2); /* Y */
    NCL_CHECK_EQ_INT(ncl_meldas_axis(3, true), 4); /* Z */
    NCL_CHECK_EQ_INT(ncl_meldas_axis(4, true), 8);
    NCL_CHECK_EQ_INT(ncl_meldas_axis(1, false), 1);
    NCL_CHECK_EQ_INT(ncl_meldas_axis(3, false), 3);
    NCL_CHECK_EQ_INT(ncl_meldas_axis(0, true), 0);
    NCL_CHECK_EQ_INT(ncl_meldas_axis(9, true), 0);
}

/* =========================================================== the machine == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    bool        serve;
    int         requests;
    int         fails_to_skip;
    uint32_t    last_command;
    uint32_t    last_subcode;
    uint32_t    last_address;
} meldas_machine;

static size_t machine_reply(meldas_machine *machine, const uint8_t *request,
                            size_t request_len, uint8_t *out, size_t cap)
{
    uint32_t id = (uint32_t)request[16] | ((uint32_t)request[17] << 8) |
                  ((uint32_t)request[18] << 16) | ((uint32_t)request[19] << 24);
    uint32_t command = (uint32_t)request[56] | ((uint32_t)request[57] << 8) |
                       ((uint32_t)request[58] << 16) |
                       ((uint32_t)request[59] << 24);
    uint32_t subcode = (uint32_t)request[60] | ((uint32_t)request[61] << 8) |
                       ((uint32_t)request[62] << 16) |
                       ((uint32_t)request[63] << 24);
    uint32_t address = (uint32_t)request[68] | ((uint32_t)request[69] << 8) |
                       ((uint32_t)request[70] << 16) |
                       ((uint32_t)request[71] << 24);
    size_t total = 48;
    size_t i;

    if (cap < total) {
        return 0;
    }
    memset(out, 0, total);
    memcpy(out, "GIOP", 4);
    out[4] = 0x01;
    out[6] = 0x01;
    out[7] = 0x01; /* Reply */
    out[8] = 36;   /* the body length */
    out[16] = (uint8_t)id;
    out[17] = (uint8_t)(id >> 8);
    out[18] = (uint8_t)(id >> 16);
    out[19] = (uint8_t)(id >> 24);

    machine->requests++;
    machine->last_command = command;
    machine->last_subcode = subcode;
    machine->last_address = address;
    if (command == 0x25 && subcode == 2) {
        /* 机械坐标：CString，轴按位编码（X=1, Y=2, Z=4） */
        const char *text = address == 1   ? "123.456"
                           : address == 2 ? "-1.5"
                           : address == 4 ? "0.001"
                                          : "";

        out[28] = NCL_MELDAS_TYPE_STRING;
        out[32] = 16; /* the size the machine reports */
        out[36] = (uint8_t)strlen(text);
        memcpy(out + 40, text, strlen(text));
        return total;
    }
    if (command == 0x3f && subcode == 4) { /* 主轴负载 */
        out[28] = NCL_MELDAS_TYPE_INT32;
        out[36] = 42;
        return total;
    }
    if (command == 0x7e && subcode == 8002) { /* 工件计数 */
        out[28] = NCL_MELDAS_TYPE_INT32;
        out[36] = 7;
        return total;
    }
    if (command == 0x03 && subcode == 0x02) { /* 机械坐标（DOUBLE 形式） */
        uint64_t raw = 0;
        double value = -2.25;

        memcpy(&raw, &value, sizeof(raw));
        out[28] = NCL_MELDAS_TYPE_DOUBLE;
        for (i = 0; i < 8; i++) {
            out[36 + i] = (uint8_t)(raw >> (8 * i));
        }
        return total;
    }
    if (command == 0x23 && subcode == 0x0a) { /* 运行状态 */
        out[28] = NCL_MELDAS_TYPE_BYTE;
        out[36] = 3;
        return total;
    }
    /* Anything else: the "no data" answer, as a machine does for an operation
     * it does not have. */
    out[8] = 28;
    memcpy(out + 28, "IDL", 3);
    return 40;
}

static void machine_main(void *arg)
{
    meldas_machine *machine = (meldas_machine *)arg;

    while (!machine->stop) {
        ncl_socket *peer = ncl_socket_accept(machine->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t head[12];
            uint8_t request[256];
            uint8_t reply[64];
            size_t body_len;
            size_t reply_len;

            if (ncl_socket_recv_exact(peer, head, 12, 2000) != NCL_OK) {
                break;
            }
            body_len = (size_t)head[8] | ((size_t)head[9] << 8) |
                       ((size_t)head[10] << 16) | ((size_t)head[11] << 24);
            if (body_len == 0 || 12u + body_len > sizeof(request)) {
                break;
            }
            memcpy(request, head, 12);
            if (ncl_socket_recv_exact(peer, request + 12, body_len, 2000) !=
                NCL_OK) {
                break;
            }
            if (machine->fails_to_skip > 0) {
                machine->fails_to_skip--;
                ncl_sleep_millis(200);
                break;
            }
            reply_len = machine_reply(machine, request, 12 + body_len, reply,
                                      sizeof(reply));
            if (reply_len == 0 || ncl_socket_send(peer, reply, reply_len) != NCL_OK) {
                break;
            }
        }
        ncl_socket_close(peer);
    }
}

static meldas_machine *machine_start(void)
{
    meldas_machine *machine =
        (meldas_machine *)ncl_mem_calloc(1, sizeof(*machine));

    if (machine == NULL) {
        return NULL;
    }
    machine->listener = ncl_socket_listen(0, NULL, 0);
    if (machine->listener == NULL) {
        ncl_free_safe(machine);
        return NULL;
    }
    machine->port = ncl_socket_local_port(machine->listener);
    machine->thread = ncl_thread_start(machine_main, machine);
    if (machine->thread == NULL) {
        ncl_socket_close(machine->listener);
        ncl_free_safe(machine);
        return NULL;
    }
    return machine;
}

static void machine_stop(meldas_machine *machine)
{
    if (machine == NULL) {
        return;
    }
    machine->stop = true;
    ncl_thread_join(machine->thread);
    ncl_socket_close(machine->listener);
    ncl_free_safe(machine);
}

static ncl_driver *meldas_driver(meldas_machine *machine, const char *extra)
{
    ncl_driver *driver = ncl_driver_create("meldas");
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":600%s}",
                            machine->port, extra != NULL ? extra : "");
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

static ncl_err read_area(ncl_driver *driver, const char *area, long long offset,
                         const char *dtype, ncl_json **value)
{
    ncl_strbuf json;
    ncl_json *node;
    ncl_address address;
    ncl_err err;

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json, "{\"area\":\"%s\",\"offset\":%lld%s%s%s}",
                            area, offset, dtype != NULL ? ",\"dtype\":\"" : "",
                            dtype != NULL ? dtype : "", dtype != NULL ? "\"" : "");
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

static void test_against_a_machine(void)
{
    meldas_machine *machine = machine_start();
    ncl_driver *driver;
    ncl_json *value = NULL;
    ncl_err err;

    NCL_TEST_CASE("a coordinate comes back as text and reaches the model as a number");
    NCL_CHECK(machine != NULL);
    if (machine == NULL) {
        return;
    }
    driver = meldas_driver(machine, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        machine_stop(machine);
        return;
    }
    NCL_CHECK_EQ_INT(read_area(driver, "machine_position", 1, "float64", &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 123.456);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(machine->last_command, 0x25);
    NCL_CHECK_EQ_INT(machine->last_subcode, 2);
    NCL_CHECK_EQ_INT(machine->last_address, 1); /* X, bit coded */

    NCL_TEST_CASE("the second axis is bit coded too");
    NCL_CHECK_EQ_INT(read_area(driver, "machine_position", 2, "float64", &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == -1.5);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(machine->last_address, 2); /* Y */

    NCL_TEST_CASE("integers and bytes decode by their marker");
    NCL_CHECK_EQ_INT(read_area(driver, "spindle_load", 0, "int32", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 42);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(read_area(driver, "part_count", 0, "int32", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 7);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(read_area(driver, "run_status", 0, "byte", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 3);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a DOUBLE answer is little endian on the wire");
    /* the 0x03 family addresses an axis too, so the offset is 1..n */
    NCL_CHECK_EQ_INT(read_area(driver, "machine_position_d", 1, "float64", &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == -2.25);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a command the machine does not have answers IDL, not an error");
    NCL_CHECK_EQ_INT(read_area(driver, "tool_life", 0, "int32", &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_type_of(value), NCL_JSON_NULL);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("an unknown command name is a point map error");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(read_area(driver, "no_such_command", 0,
                                                     NULL, &value)),
                     3);

    NCL_TEST_CASE("writing is refused: the frame layout is not documented");
    NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, NULL, NULL),
                     NCL_ERR_NOT_SUPPORTED);

    NCL_TEST_CASE("the raw hatch takes the five request fields");
    {
        ncl_driver_result out;
        /* command 0x3f, sub code 4, count 1, address 0, want INT32 */
        const uint8_t raw[] = {0x3f, 0, 0, 0, 0x04, 0, 0, 0, 1, 0, 0, 0,
                               0, 0, 0, 0, NCL_MELDAS_TYPE_INT32, 0, 0, 0};

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, raw, sizeof(raw), &out),
                         NCL_OK);
        NCL_CHECK(out.success);
        NCL_CHECK_EQ_STR(ncl_json_as_string(out.value),
                         "032a00000000000000"); /* marker + 42 */
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("the session reopens on demand");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    NCL_CHECK_EQ_INT(read_area(driver, "machine_position", 1, "float64", &value),
                     NCL_OK);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK(driver->ops->is_connected(driver));

    NCL_TEST_CASE("a machine that stops answering is a transport failure");
    machine->fails_to_skip = 1;
    err = read_area(driver, "machine_position", 1, "float64", &value);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);

    driver->ops->destroy(driver);
    machine_stop(machine);
}

static void test_axis_modes(void)
{
    meldas_machine *machine = machine_start();
    ncl_driver *driver;
    ncl_json *value = NULL;

    NCL_TEST_CASE("axisMode index passes the axis number through");
    NCL_CHECK(machine != NULL);
    if (machine == NULL) {
        return;
    }
    driver = meldas_driver(machine, ",\"axisMode\":\"index\"");
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        NCL_CHECK_EQ_INT(read_area(driver, "machine_position", 3, "float64",
                                   &value),
                         NCL_OK);
        ncl_json_free(value);
        NCL_CHECK_EQ_INT(machine->last_address, 3); /* index mode: as written */
        driver->ops->destroy(driver);
    }
    machine_stop(machine);

    NCL_TEST_CASE("an unknown axisMode is a configuration error");
    machine = machine_start();
    if (machine != NULL) {
        ncl_driver *other = ncl_driver_create("meldas");
        ncl_json *params = ncl_json_parse_cstr(
            "{\"host\":\"127.0.0.1\",\"axisMode\":\"sideways\"}", NULL);

        NCL_CHECK(other != NULL);
        if (other != NULL) {
            NCL_CHECK_EQ_INT(other->ops->create(other, params),
                             NCL_ERR_INVALID_ARG);
            other->ops->destroy(other);
        }
        ncl_json_free(params);
        machine_stop(machine);
    }
}

static void test_through_the_manager(void)
{
    meldas_machine *machine = machine_start();
    test_point_map *manager = test_point_map_create(ncl_meldas_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured MELDAS link reads through the point map");
    NCL_CHECK(machine != NULL && manager != NULL);
    if (machine == NULL || manager == NULL) {
        machine_stop(machine);
        test_point_map_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"cnc\",\"path\":\"/CNC\","
                            "\"type\":\"meldas\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":600},"
                            "\"points\":["
                            "{\"path\":\"/CNC/X\",\"addr\":"
                            "{\"area\":\"machine_position\",\"offset\":1,"
                            "\"dtype\":\"float64\"}},"
                            "{\"path\":\"/CNC/LOAD\",\"addr\":"
                            "{\"area\":\"spindle_load\",\"offset\":0,"
                            "\"dtype\":\"int32\"}}]}",
                            machine->port);
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

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/X", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 123.456);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/LOAD", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 42);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    machine_stop(machine);
}

NCL_TEST_MAIN_BEGIN()
    test_request();
    test_reply();
    test_names();
    test_against_a_machine();
    test_axis_modes();
    test_through_the_manager();
NCL_TEST_MAIN_END()
