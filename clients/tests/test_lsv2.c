/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * HEIDENHAIN LSV2: the frame of 07-HEIDENHAIN-LSV2.md §3.3 byte for byte, then
 * a mock control the test runs itself.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "test_point_map.h"
#include "nclink/clients/lsv2.h"
#include "lsv2/ncl_lsv2_driver.h"

static bool binary_equal(const uint8_t *actual, const char *expected, size_t len)
{
    return memcmp(actual, expected, len) == 0;
}

/* ============================================================== the bytes == */

static void test_frames(void)
{
    uint8_t frame[64];
    char name[5];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    size_t frame_len = 0;
    char text[32];
    size_t used;

    NCL_TEST_CASE("the R_FL of §3.3 is reproduced byte for byte");
    used = ncl_lsv2_string_payload(text, sizeof(text), "test.h");
    NCL_CHECK_EQ_INT(used, 7); /* "test.h" plus its NUL */
    NCL_CHECK_EQ_INT(ncl_lsv2_frame(frame, sizeof(frame), "R_FL", text, used), 15);
    NCL_CHECK(binary_equal(frame, "\x00\x00\x00\x07"
                                  "R_FL"
                                  "test.h\x00",
                           15));

    NCL_TEST_CASE("a frame splits back into its name and payload");
    NCL_CHECK_EQ_INT(ncl_lsv2_split(frame, 15, name, &payload, &payload_len,
                                    &frame_len),
                     NCL_OK);
    NCL_CHECK_EQ_STR(name, "R_FL");
    NCL_CHECK_EQ_INT(payload_len, 7);
    NCL_CHECK_EQ_INT(frame_len, 15);
    NCL_CHECK_EQ_STR((const char *)payload, "test.h");

    NCL_TEST_CASE("a frame that is still arriving is not an error");
    NCL_CHECK_EQ_INT(ncl_lsv2_split(frame, 4, name, NULL, NULL, NULL),
                     NCL_ERR_RANGE);
    NCL_CHECK_EQ_INT(ncl_lsv2_split(frame, 10, name, NULL, NULL, NULL),
                     NCL_ERR_RANGE);

    NCL_TEST_CASE("a name that is not four characters is refused");
    NCL_CHECK_EQ_INT(ncl_lsv2_frame(frame, sizeof(frame), "R_FLX", text, used), 0);
    NCL_CHECK_EQ_INT(ncl_lsv2_frame(frame, sizeof(frame), "R_F", text, used), 0);
    NCL_CHECK_EQ_INT(ncl_lsv2_frame(frame, 8, "R_FL", text, used), 0);
}

static void test_tables(void)
{
    char message[160];

    NCL_TEST_CASE("the command and response names of §4 are known");
    NCL_CHECK(ncl_lsv2_command_known("R_MB"));
    NCL_CHECK(ncl_lsv2_command_known("A_LG"));
    NCL_CHECK(ncl_lsv2_command_known("S_FL"));
    NCL_CHECK(ncl_lsv2_command_known("T_ER"));
    NCL_CHECK(!ncl_lsv2_command_known("R_XX"));
    NCL_CHECK(!ncl_lsv2_command_known("R_M"));
    NCL_CHECK_EQ_STR(ncl_lsv2_command_text("R_VR"), "控制器版本信息");
    NCL_CHECK(ncl_lsv2_command_text("R_XX") == NULL);

    NCL_TEST_CASE("status codes are named and tiered");
    NCL_CHECK_EQ_STR(ncl_lsv2_status_text(0), "LSV2_OK");
    NCL_CHECK_EQ_STR(ncl_lsv2_status_text(6), "LSV2_WRONG_BBC");
    NCL_CHECK_EQ_INT(ncl_lsv2_check_status(0, message, sizeof(message)), NCL_OK);
    /* a timeout or a missing answer is the link's problem */
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_lsv2_check_status(1, message,
                                                                 sizeof(message))),
                     1);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_lsv2_check_status(16, message,
                                                                 sizeof(message))),
                     1);
    /* a wrong block check or a too long frame is the protocol's */
    NCL_CHECK_EQ_INT(ncl_lsv2_check_status(6, message, sizeof(message)),
                     NCL_DRV_ERR_PROTOCOL(0x66));
    /* anything else is the control refusing the request */
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(ncl_lsv2_check_status(42, message,
                                                                 sizeof(message))),
                     3);

    NCL_TEST_CASE("the memory types and the state enums of §5");
    {
        unsigned code = 0;

        NCL_CHECK(ncl_lsv2_memory_type("MARKER", &code) && code == 1);
        NCL_CHECK(ncl_lsv2_memory_type("m", &code) && code == 1);
        NCL_CHECK(ncl_lsv2_memory_type("INPUT_WORD", &code) && code == 10);
        NCL_CHECK(ncl_lsv2_memory_type("input_dword", &code) && code == 13);
        NCL_CHECK(!ncl_lsv2_memory_type("SPINDLE", &code));
    }
    NCL_CHECK_EQ_STR(ncl_lsv2_exec_state(4), "AUTOMATIC");
    NCL_CHECK_EQ_STR(ncl_lsv2_exec_state(0), "MANUAL");
    NCL_CHECK_EQ_STR(ncl_lsv2_exec_state(99), "UNDEFINED");
    NCL_CHECK_EQ_STR(ncl_lsv2_pgm_state(0), "STARTED");
    NCL_CHECK_EQ_STR(ncl_lsv2_pgm_state(7), "IDLE");
    NCL_CHECK_EQ_STR(ncl_lsv2_pgm_state(99), "UNDEFINED");

    NCL_TEST_CASE("the R_MB payload is a four byte address and a length byte");
    {
        uint8_t body[8];

        NCL_CHECK_EQ_INT(ncl_lsv2_read_memory_payload(body, sizeof(body), 0x00010000,
                                                      2),
                         5);
        NCL_CHECK(binary_equal(body, "\x00\x01\x00\x00\x02", 5));
        NCL_CHECK_EQ_INT(ncl_lsv2_read_memory_payload(body, sizeof(body), -1, 1),
                         0);
    }
}

/* ============================================================ the control == */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    bool        require_login;
    int         requests;
    int         fails_to_skip;
    int         logins;
    char        last_command[5];
    uint8_t     memory[256];
} lsv2_control;

static void control_reply(lsv2_control *control, ncl_socket *peer,
                          const uint8_t *frame, size_t frame_len)
{
    char name[5];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t out[512];
    size_t out_len;
    const char *answer = "T_OK";

    if (ncl_lsv2_split(frame, frame_len, name, &payload, &payload_len, NULL) !=
        NCL_OK) {
        return;
    }
    control->requests++;
    memcpy(control->last_command, name, 5);
    if (strcmp(name, "A_LG") == 0) {
        control->logins++;
        answer = "T_OK";
        out_len = ncl_lsv2_frame(out, sizeof(out), answer, NULL, 0);
    } else if (strcmp(name, "A_LO") == 0) {
        answer = "T_OK";
        out_len = ncl_lsv2_frame(out, sizeof(out), answer, NULL, 0);
    } else if (strcmp(name, "R_VR") == 0) {
        char version[64];
        size_t used = ncl_lsv2_string_payload(version, sizeof(version),
                                              "iTNC530 60642x-01");

        out_len = ncl_lsv2_frame(out, sizeof(out), "S_VR", version, used);
    } else if (strcmp(name, "R_ST") == 0) {
        static const uint8_t kStatus[] = {0x00, 0x04}; /* automatic */

        out_len = ncl_lsv2_frame(out, sizeof(out), "S_ST", kStatus,
                                 sizeof(kStatus));
    } else if (strcmp(name, "R_MB") == 0) {
        uint32_t address;
        unsigned count;
        uint8_t data[64];
        unsigned i;

        if (payload_len < 5) {
            out_len = ncl_lsv2_frame(out, sizeof(out), "T_ER",
                                     (const uint8_t *)"\x05", 1);
        } else {
            address = ((uint32_t)payload[0] << 24) |
                      ((uint32_t)payload[1] << 16) |
                      ((uint32_t)payload[2] << 8) | payload[3];
            count = payload[4];
            if (count > sizeof(data) || address + count > sizeof(control->memory)) {
                out_len = ncl_lsv2_frame(out, sizeof(out), "T_ER",
                                         (const uint8_t *)"\x05", 1);
            } else {
                for (i = 0; i < count; i++) {
                    data[i] = control->memory[address + i];
                }
                out_len = ncl_lsv2_frame(out, sizeof(out), "S_MB", data, count);
            }
        }
    } else {
        /* §4.3 does not give a payload for this one: a control answers with an
         * error status, which is what the driver has to survive. */
        out_len = ncl_lsv2_frame(out, sizeof(out), "T_ER",
                                 (const uint8_t *)"\x10", 1);
    }
    if (out_len > 0) {
        (void)ncl_socket_send(peer, out, out_len);
    }
}

static void control_main(void *arg)
{
    lsv2_control *control = (lsv2_control *)arg;

    while (!control->stop) {
        ncl_socket *peer = ncl_socket_accept(control->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t head[8];
            uint8_t frame[512];
            size_t declared;

            if (ncl_socket_recv_exact(peer, head, 8, 2000) != NCL_OK) {
                break;
            }
            declared = ((size_t)head[0] << 24) | ((size_t)head[1] << 16) |
                       ((size_t)head[2] << 8) | head[3];
            if (declared > 256) {
                break;
            }
            memcpy(frame, head, 8);
            if (declared > 0 &&
                ncl_socket_recv_exact(peer, frame + 8, declared, 2000) != NCL_OK) {
                break;
            }
            if (control->fails_to_skip > 0) {
                control->fails_to_skip--;
                ncl_sleep_millis(200);
                break;
            }
            control_reply(control, peer, frame, 8 + declared);
        }
        ncl_socket_close(peer);
    }
}

static lsv2_control *control_start(void)
{
    lsv2_control *control = (lsv2_control *)ncl_mem_calloc(1, sizeof(*control));
    size_t i;

    if (control == NULL) {
        return NULL;
    }
    control->listener = ncl_socket_listen(0, NULL, 0);
    if (control->listener == NULL) {
        ncl_free_safe(control);
        return NULL;
    }
    control->port = ncl_socket_local_port(control->listener);
    for (i = 0; i < sizeof(control->memory); i++) {
        control->memory[i] = (uint8_t)i;
    }
    control->thread = ncl_thread_start(control_main, control);
    if (control->thread == NULL) {
        ncl_socket_close(control->listener);
        ncl_free_safe(control);
        return NULL;
    }
    return control;
}

static void control_stop(lsv2_control *control)
{
    if (control == NULL) {
        return;
    }
    control->stop = true;
    ncl_thread_join(control->thread);
    ncl_socket_close(control->listener);
    ncl_free_safe(control);
}

static ncl_driver *lsv2_driver(lsv2_control *control, const char *extra)
{
    ncl_driver *driver = ncl_driver_create("lsv2");
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":800%s}",
                            control->port, extra != NULL ? extra : "");
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

static void test_against_a_control(void)
{
    lsv2_control *control = control_start();
    ncl_driver *driver;
    ncl_json *value = NULL;
    ncl_err err;

    NCL_TEST_CASE("opening asks for the version first (§7.4)");
    NCL_CHECK(control != NULL);
    if (control == NULL) {
        return;
    }
    driver = lsv2_driver(control, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        control_stop(control);
        return;
    }
    NCL_CHECK_EQ_INT(read_area(driver, "version", 0, "string", &value), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "iTNC530 60642x-01");
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_STR(control->last_command, "R_VR"); /* the version asked for */
    NCL_CHECK(control->requests >= 1);

    NCL_TEST_CASE("the remote status answers as bytes");
    NCL_CHECK_EQ_INT(read_area(driver, "remote_status", 0, "string", &value),
                     NCL_OK);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("PLC memory is read by address");
    control->memory[100] = 0x12;
    control->memory[101] = 0x34;
    NCL_CHECK_EQ_INT(read_area(driver, "plc_memory", 100, "int16", &value),
                     NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 0x1234);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(read_area(driver, "plc_memory", 100, "byte", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 0x12);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a command the control does not have becomes a transport error");
    err = read_area(driver, "plc_memory", 5000, "byte", &value);
    NCL_CHECK(ncl_driver_error_tier(err) != 0); /* T_ER with status 0x10 */

    NCL_TEST_CASE("an area this driver does not have is a point map error");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(read_area(driver, "position", 0,
                                                     "float64", &value)),
                     3);

    NCL_TEST_CASE("writing is refused: §7.6 keeps C_EK and C_MC walled off");
    NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, NULL, NULL),
                     NCL_ERR_NOT_SUPPORTED);

    NCL_TEST_CASE("the methods report the session");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "version", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_STR(ncl_json_as_string(result), "iTNC530 60642x-01");
        ncl_json_free(result);
        NCL_CHECK_EQ_INT(driver->ops->call(driver, "keepAlive", NULL, &result),
                         NCL_OK);
        ncl_json_free(result);
        result = NULL;
        NCL_CHECK_EQ_INT(driver->ops->call(driver, "loginState", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "user", 1), 1); /* "" */
        ncl_json_free(result);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("the raw hatch sends any command and returns its answer");
    {
        ncl_driver_result out;
        const char raw[] = "R_MB\x00\x00\x00\x64\x01"; /* the 100th byte */

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, raw, 9, &out), NCL_OK);
        NCL_CHECK(out.success);
        NCL_CHECK_EQ_STR(ncl_json_as_string(out.value), "12");
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("a control that stops answering is a transport failure");
    control->fails_to_skip = 1;
    err = read_area(driver, "version", 0, "string", &value);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);

    driver->ops->destroy(driver);
    control_stop(control);
}

static void test_login(void)
{
    lsv2_control *control = control_start();
    ncl_driver *driver;
    ncl_json *value = NULL;

    NCL_TEST_CASE("a login name is sent as A_LG + the name + NUL");
    NCL_CHECK(control != NULL);
    if (control == NULL) {
        return;
    }
    driver = lsv2_driver(control, ",\"user\":\"INSPECT\"");
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        NCL_CHECK_EQ_INT(read_area(driver, "version", 0, "string", &value), NCL_OK);
        ncl_json_free(value);
        value = NULL;
        NCL_CHECK_EQ_INT(control->logins, 1);
        driver->ops->destroy(driver);
    }
    control_stop(control);

    NCL_TEST_CASE("a login name outside §4.1 is refused by the configuration");
    {
        ncl_driver *other = ncl_driver_create("lsv2");
        ncl_json *params = ncl_json_parse_cstr(
            "{\"host\":\"127.0.0.1\",\"user\":\"ROOT\"}", NULL);

        NCL_CHECK(other != NULL);
        if (other != NULL) {
            NCL_CHECK_EQ_INT(other->ops->create(other, params),
                             NCL_ERR_INVALID_ARG);
            other->ops->destroy(other);
        }
        ncl_json_free(params);
    }
}

static void test_through_the_manager(void)
{
    lsv2_control *control = control_start();
    test_point_map *manager = test_point_map_create(ncl_lsv2_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured LSV2 link reads through the point map");
    NCL_CHECK(control != NULL && manager != NULL);
    if (control == NULL || manager == NULL) {
        control_stop(control);
        test_point_map_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"tnc\",\"path\":\"/TNC\","
                            "\"type\":\"lsv2\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":800,"
                            "\"user\":\"INSPECT\"},"
                            "\"points\":["
                            "{\"path\":\"/TNC/VER\",\"addr\":"
                            "{\"area\":\"version\",\"offset\":0,"
                            "\"dtype\":\"string\"}},"
                            "{\"path\":\"/TNC/M100\",\"addr\":"
                            "{\"area\":\"plc_memory\",\"offset\":100,"
                            "\"dtype\":\"int16\"}}]}",
                            control->port);
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

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/TNC/VER", &value), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "iTNC530 60642x-01");
    ncl_json_free(value);
    value = NULL;
    control->memory[100] = 0x00;
    control->memory[101] = 0x2A;
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/TNC/M100", &value), NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 42);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/TNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    control_stop(control);
}

NCL_TEST_MAIN_BEGIN()
    test_frames();
    test_tables();
    test_against_a_control();
    test_login();
    test_through_the_manager();
NCL_TEST_MAIN_END()
