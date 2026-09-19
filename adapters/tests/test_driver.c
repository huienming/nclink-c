/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The driver layer: type names, error tiers, the response envelope, the
 * unified address parser and the protocol registry, exercised through the
 * "mock" driver so the whole interface runs without a wire.
 */
#include <stdio.h>

#include "ncl_test.h"

#include "nclink_adapter/ncl_driver.h"
#include "mock/ncl_mock_driver.h"

/* ------------------------------------------------------------- helpers --- */

static ncl_err parse_addr(const char *spec, ncl_address *addr)
{
    ncl_json *node = ncl_json_parse_cstr(spec, NULL);
    ncl_err err;

    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, addr);
    ncl_json_free(node);
    return err;
}

static ncl_err read_spec(ncl_driver *driver, const char *spec, ncl_json **value)
{
    ncl_address addr;
    ncl_err err = parse_addr(spec, &addr);

    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_read_one(driver, &addr, value);
    ncl_address_clear(&addr);
    return err;
}

static ncl_err write_spec(ncl_driver *driver, const char *spec,
                          const ncl_json *value)
{
    ncl_address addr;
    ncl_err err = parse_addr(spec, &addr);

    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_write_one(driver, &addr, value);
    ncl_address_clear(&addr);
    return err;
}

static ncl_driver *make_mock(const ncl_json *parameters)
{
    ncl_driver *driver = ncl_driver_create("mock");

    if (driver == NULL) {
        return NULL;
    }
    if (driver->ops->create(driver, parameters) != NCL_OK) {
        driver->ops->destroy(driver);
        return NULL;
    }
    return driver;
}

static long long read_int(ncl_driver *driver, const char *spec, ncl_err *err)
{
    ncl_json *value = NULL;
    long long number = -1;

    *err = read_spec(driver, spec, &value);
    if (*err == NCL_OK && !ncl_json_as_int(value, &number)) {
        *err = NCL_ERR_INVALID_VALUE;
    }
    ncl_json_free(value);
    return number;
}

static int  g_events;
static char g_event_id[64];

static void on_event(void *user, const char *event_id, const ncl_json *event)
{
    (void)user;
    (void)event;
    g_events++;
    snprintf(g_event_id, sizeof(g_event_id), "%s", event_id != NULL ? event_id : "");
}

/* ---------------------------------------------------------------- types -- */

static void test_types(void)
{
    ncl_dtype dtype = NCL_DTYPE_STRING;

    NCL_TEST_CASE("data types");
    NCL_CHECK_EQ_STR(ncl_dtype_name(NCL_DTYPE_BIT), "bit");
    NCL_CHECK_EQ_STR(ncl_dtype_name(NCL_DTYPE_FLOAT64), "float64");
    NCL_CHECK_EQ_STR(ncl_dtype_name((ncl_dtype)99), "?");

    NCL_CHECK(ncl_dtype_parse("INT16", &dtype) && dtype == NCL_DTYPE_INT16);
    NCL_CHECK(ncl_dtype_parse("real", &dtype) && dtype == NCL_DTYPE_FLOAT32);
    NCL_CHECK(ncl_dtype_parse("bool", &dtype) && dtype == NCL_DTYPE_BIT);
    NCL_CHECK(!ncl_dtype_parse("nibble", &dtype));
    NCL_CHECK(!ncl_dtype_parse("", &dtype));
}

/* --------------------------------------------------------------- errors -- */

static void test_errors(void)
{
    NCL_TEST_CASE("three error tiers stay apart");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(0), 0);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(NCL_DRV_ERR_TRANSPORT(1)), 1);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(NCL_DRV_ERR_PROTOCOL(20)), 2);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(NCL_DRV_ERR_BUSINESS(1)), 3);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(NCL_ERR_INVALID_ARG), -1);
    NCL_CHECK_EQ_INT(NCL_DRV_ERR_PROTOCOL(20), 0x20000014);
    NCL_CHECK_EQ_STR(ncl_driver_error_tier_name(NCL_DRV_ERR_TRANSPORT(3)),
                     "transport");
    NCL_CHECK_EQ_STR(ncl_driver_error_tier_name(0), "success");
    NCL_CHECK_EQ_STR(ncl_driver_error_tier_name(NCL_ERR_NOMEM), "local");

    NCL_TEST_CASE("response envelope");
    {
        ncl_driver_result result;
        const uint8_t frame[] = {0x01, 0x02, 0xFF};

        ncl_driver_result_init(&result);
        NCL_CHECK_EQ_INT(result.code, 0);
        ncl_driver_result_ok(&result, ncl_json_new_int(42));
        NCL_CHECK(result.success);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result.value, "nope", 42), 42);
        ncl_driver_result_set_raw(&result, frame, sizeof(frame));
        NCL_CHECK_EQ_INT(result.raw_len, 3);
        NCL_CHECK(result.raw[2] == 0xFF);

        ncl_driver_result_fail(&result, NCL_DRV_ERR_PROTOCOL(7), "bad reply %d", 7);
        NCL_CHECK(!result.success);
        NCL_CHECK_EQ_INT(ncl_driver_error_tier(result.code), 2);
        NCL_CHECK(result.value == NULL);
        NCL_CHECK_EQ_INT(result.raw_len, 3); /* the reply survives for the log */
        NCL_CHECK_EQ_STR(result.message, "bad reply 7");
        ncl_driver_result_free(&result);
        NCL_CHECK_EQ_INT(result.code, 0);
    }
}

/* ------------------------------------------------------------- address --- */

static void test_address(void)
{
    ncl_address addr;
    char *text;

    NCL_TEST_CASE("address shorthand");
    NCL_CHECK_EQ_INT(parse_addr("\"D100\"", &addr), NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "D");
    NCL_CHECK_EQ_INT(addr.offset, 100);
    NCL_CHECK_EQ_INT(addr.bit, -1);
    NCL_CHECK_EQ_INT(addr.length, 1);
    NCL_CHECK_EQ_INT(addr.dtype, NCL_DTYPE_INT16);
    ncl_address_clear(&addr);

    NCL_CHECK_EQ_INT(parse_addr("\"M10.3\"", &addr), NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "M");
    NCL_CHECK_EQ_INT(addr.offset, 10);
    NCL_CHECK_EQ_INT(addr.bit, 3);
    NCL_CHECK_EQ_INT(addr.dtype, NCL_DTYPE_BIT);
    ncl_address_clear(&addr);

    NCL_CHECK_EQ_INT(parse_addr("\"CIO\"", &addr), NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "CIO");
    NCL_CHECK_EQ_INT(addr.offset, 0);
    ncl_address_clear(&addr);

    /* Modbus writes its areas the traditional way: 4x = holding registers */
    NCL_CHECK_EQ_INT(parse_addr("\"4x12\"", &addr), NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "4x");
    NCL_CHECK_EQ_INT(addr.offset, 12);
    ncl_address_clear(&addr);
    NCL_CHECK_EQ_INT(parse_addr("\"1x8\"", &addr), NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "1x");
    NCL_CHECK_EQ_INT(addr.offset, 8);
    ncl_address_clear(&addr);
    NCL_CHECK_EQ_INT(parse_addr("\"4\"", &addr), NCL_OK);
    NCL_CHECK(addr.area == NULL);
    NCL_CHECK_EQ_INT(addr.offset, 4);
    ncl_address_clear(&addr);

    NCL_CHECK_EQ_INT(parse_addr("\"\"", &addr), NCL_ERR_PARSE);
    NCL_CHECK_EQ_INT(parse_addr("\"D1x\"", &addr), NCL_ERR_PARSE);
    NCL_CHECK_EQ_INT(parse_addr("\"D1.\"", &addr), NCL_ERR_PARSE);
    NCL_CHECK_EQ_INT(parse_addr("\"M1.99\"", &addr), NCL_ERR_RANGE);

    NCL_TEST_CASE("address object form");
    NCL_CHECK_EQ_INT(parse_addr("{\"area\":\"DB\",\"offset\":10,\"bit\":5,"
                                "\"length\":4,\"dtype\":\"uint16\"}",
                                &addr),
                     NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "DB");
    NCL_CHECK_EQ_INT(addr.offset, 10);
    NCL_CHECK_EQ_INT(addr.bit, 5);
    NCL_CHECK_EQ_INT(addr.length, 4);
    NCL_CHECK_EQ_INT(addr.dtype, NCL_DTYPE_BIT); /* a bit index wins */

    text = ncl_address_to_text(&addr);
    NCL_CHECK_EQ_STR(text, "DB10.5 x4 bit");
    ncl_free_safe(text);
    ncl_address_clear(&addr);

    NCL_CHECK_EQ_INT(parse_addr("{\"area\":\"D10\",\"dtype\":\"float\"}", &addr),
                     NCL_OK);
    NCL_CHECK_EQ_INT(addr.offset, 10);
    NCL_CHECK_EQ_INT(addr.dtype, NCL_DTYPE_FLOAT32);
    ncl_address_clear(&addr);

    /* "offset" present: the area is taken literally. */
    NCL_CHECK_EQ_INT(parse_addr("{\"area\":\"D10\",\"offset\":3}", &addr),
                     NCL_OK);
    NCL_CHECK_EQ_STR(addr.area, "D10");
    NCL_CHECK_EQ_INT(addr.offset, 3);
    ncl_address_clear(&addr);

    NCL_CHECK_EQ_INT(parse_addr("{\"area\":\"D\",\"offset\":1,\"length\":0}",
                                &addr),
                     NCL_ERR_RANGE);
    NCL_CHECK_EQ_INT(parse_addr("{\"area\":\"D\",\"dtype\":\"nibble\"}", &addr),
                     NCL_ERR_INVALID_DATA_TYPE);
    NCL_CHECK_EQ_INT(parse_addr("{\"offset\":1}", &addr), NCL_ERR_INVALID_ARG);
    NCL_CHECK_EQ_INT(parse_addr("42", &addr), NCL_ERR_PARSE);
    NCL_CHECK_EQ_INT(parse_addr("null", &addr), NCL_ERR_PARSE);
}

/* ------------------------------------------------------------ registry --- */

static void test_registry(void)
{
    ncl_driver *driver;
    size_t before;

    NCL_TEST_CASE("protocol registry");
    ncl_driver_register_builtin();
    before = ncl_driver_protocol_count();
    NCL_CHECK(before >= 1);

    driver = ncl_driver_create("mock");
    NCL_CHECK(driver != NULL);
    NCL_CHECK_EQ_STR(ncl_driver_protocol(driver), "mock");
    NCL_CHECK(ncl_driver_ops_of(driver) != NULL);
    if (driver != NULL) {
        driver->ops->destroy(driver);
    }

    NCL_CHECK(ncl_driver_create("MOCK") != NULL); /* names are case-insensitive */
    NCL_CHECK(ncl_driver_create("mpi") == NULL);
    driver = ncl_driver_create("MOCK");
    if (driver != NULL) {
        driver->ops->destroy(driver);
    }

    NCL_CHECK_EQ_INT(ncl_driver_register_protocol("mock", ncl_mock_driver_create),
                     NCL_ERR_EXISTS);
    NCL_CHECK_EQ_INT(ncl_driver_register_protocol("  ", ncl_mock_driver_create),
                     NCL_ERR_INVALID_ARG);
    NCL_CHECK_EQ_INT(ncl_driver_register_protocol("test_only", NULL),
                     NCL_ERR_INVALID_ARG);
    NCL_CHECK_EQ_INT(ncl_driver_register_protocol("test_only",
                                                  ncl_mock_driver_create),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_driver_protocol_count(), before + 1);
    NCL_CHECK(ncl_driver_create("test_only") != NULL);
}

/* --------------------------------------------------------------- mock ---- */

static void test_mock_memory(void)
{
    ncl_json *params = ncl_json_parse_cstr(
        "{\"points\":[{\"area\":\"D\",\"offset\":100,\"dtype\":\"int16\","
        "\"value\":1234},{\"area\":\"D\",\"offset\":300,"
        "\"dtype\":\"string\",\"value\":\"hello\"}]}",
        NULL);
    ncl_driver *driver = make_mock(params);
    ncl_json *value = NULL;
    long long number = 0;
    ncl_err err;

    ncl_json_free(params);
    NCL_TEST_CASE("mock: the session opens on demand");
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        return;
    }
    NCL_CHECK(!driver->ops->is_connected(driver));

    /* read_batch without a session is a transport failure... */
    {
        ncl_address addr;
        ncl_json *values = NULL;

        NCL_CHECK_EQ_INT(parse_addr("\"D100\"", &addr), NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, &addr, 1, &values),
                         NCL_DRV_ERR_TRANSPORT(1));
        NCL_CHECK(values == NULL);
        ncl_address_clear(&addr);
    }
    /* ... while read_one() opens it and then succeeds. */
    NCL_CHECK_EQ_INT(read_spec(driver, "\"D100\"", &value), NCL_OK);
    NCL_CHECK(driver->ops->is_connected(driver));
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 1234);
    ncl_json_free(value);

    NCL_TEST_CASE("mock: blank memory reads as zero, text keeps its value");
    number = read_int(driver, "\"D200\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 0);

    NCL_CHECK_EQ_INT(read_spec(driver,
                               "{\"area\":\"D\",\"offset\":300,"
                               "\"dtype\":\"string\"}",
                               &value),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "hello");
    ncl_json_free(value);

    NCL_TEST_CASE("mock: write, read back, and bit addressing");
    NCL_CHECK_EQ_INT(write_spec(driver, "\"D200\"", ncl_json_new_int(-7)),
                     NCL_OK);
    number = read_int(driver, "\"D200\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, -7);
    /* int16 truncates like the device would */
    NCL_CHECK_EQ_INT(write_spec(driver, "\"D201\"", ncl_json_new_int(0x12345)),
                     NCL_OK);
    number = read_int(driver, "\"D201\"", &err);
    NCL_CHECK_EQ_INT(number, 0x2345);

    NCL_CHECK_EQ_INT(write_spec(driver, "\"M10.3\"", ncl_json_new_bool(true)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(read_spec(driver, "\"M10.3\"", &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_type_of(value), NCL_JSON_BOOL);
    {
        bool set = false;

        NCL_CHECK(ncl_json_as_bool(value, &set) && set);
    }
    ncl_json_free(value);
    number = read_int(driver, "\"M10\"", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 8);
    NCL_CHECK_EQ_INT(write_spec(driver, "\"M10.3\"", ncl_json_new_bool(false)),
                     NCL_OK);
    number = read_int(driver, "\"M10\"", &err);
    NCL_CHECK_EQ_INT(number, 0);

    NCL_TEST_CASE("mock: length > 1 reads and writes an array");
    {
        ncl_address addr;
        ncl_json *values = NULL;
        ncl_json *written = ncl_json_new_array();
        ncl_json *section = ncl_json_new_array();
        size_t i;

        NCL_CHECK_EQ_INT(
            parse_addr("{\"area\":\"D\",\"offset\":400,\"length\":4}", &addr),
            NCL_OK);
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, &addr, 1, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(values), 1);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_arr_get(values, 0)), 4);
        {
            long long element = -1;
            bool is_number = ncl_json_as_int(
                ncl_json_arr_get(ncl_json_arr_get(values, 0), 2), &element);

            NCL_CHECK(is_number);
            NCL_CHECK_EQ_INT(element, 0);
        }
        ncl_json_free(values);

        for (i = 1; i <= 4; i++) {
            (void)ncl_json_arr_push(section, ncl_json_new_int((long long)i));
        }
        (void)ncl_json_arr_push(written, section);
        NCL_CHECK_EQ_INT(driver->ops->write_batch(driver, &addr, written, 1),
                         NCL_OK);
        ncl_json_free(written);
        /* the wrong shape (4 scalars for 1 address) is rejected */
        written = ncl_json_new_array();
        for (i = 1; i <= 4; i++) {
            (void)ncl_json_arr_push(written, ncl_json_new_int((long long)i));
        }
        NCL_CHECK_EQ_INT(driver->ops->write_batch(driver, &addr, written, 1),
                         NCL_ERR_INVALID_ARG);
        ncl_json_free(written);

        number = read_int(driver, "\"D402\"", &err);
        NCL_CHECK_EQ_INT(err, NCL_OK);
        NCL_CHECK_EQ_INT(number, 3);
        ncl_address_clear(&addr);
    }

    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    NCL_CHECK_EQ_INT(read_spec(driver, "\"D100\"", &value), NCL_OK);
    ncl_json_free(value);
    driver->ops->destroy(driver);
}

static void test_mock_failures(void)
{
    ncl_json *params = ncl_json_new_object();
    ncl_json *fail = ncl_json_new_object();
    ncl_driver *driver;
    ncl_json *value = NULL;

    NCL_TEST_CASE("mock: failure injection walks the tiers");
    (void)ncl_json_obj_set_int(fail, "code", NCL_DRV_ERR_PROTOCOL(20));
    (void)ncl_json_obj_set_int(fail, "count", 2);
    (void)ncl_json_obj_set(params, "fail", fail);
    driver = make_mock(params);
    ncl_json_free(params);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        return;
    }

    NCL_CHECK_EQ_INT(read_spec(driver, "\"D1\"", &value), NCL_DRV_ERR_PROTOCOL(20));
    NCL_CHECK_EQ_INT(read_spec(driver, "\"D1\"", &value), NCL_DRV_ERR_PROTOCOL(20));
    NCL_CHECK_EQ_INT(read_spec(driver, "\"D1\"", &value), NCL_OK);
    ncl_json_free(value);
    value = NULL;
    driver->ops->destroy(driver);

    NCL_TEST_CASE("mock: reading an unmapped address is a business error");
    params = ncl_json_parse_cstr("{\"unmapped\":\"error\"}", NULL);
    driver = make_mock(params);
    ncl_json_free(params);
    NCL_CHECK_EQ_INT(read_spec(driver, "\"D1\"", &value), NCL_DRV_ERR_BUSINESS(1));
    driver->ops->destroy(driver);
}

static void test_mock_call_and_events(void)
{
    ncl_driver *driver = make_mock(NULL);
    ncl_json *result = NULL;
    ncl_json *params;

    NCL_TEST_CASE("mock: call() operations");
    NCL_CHECK(driver != NULL);
    params = ncl_json_parse_cstr("{\"a\":1,\"b\":[2,3]}", NULL);
    NCL_CHECK_EQ_INT(driver->ops->call(driver, "echo", params, &result), NCL_OK);
    NCL_CHECK(result != NULL && ncl_json_equals(result, params));
    ncl_json_free(result);
    ncl_json_free(params);

    NCL_CHECK_EQ_INT(driver->ops->call(driver, "nonsense", NULL, &result),
                     NCL_DRV_ERR_PROTOCOL(1));

    NCL_TEST_CASE("mock: the event hook fires");
    g_events = 0;
    g_event_id[0] = '\0';
    driver->ops->attach_event(driver, on_event, NULL);
    params = ncl_json_parse_cstr(
        "{\"id\":\"alarm:1201\",\"event\":{\"code\":1201}}", NULL);
    NCL_CHECK_EQ_INT(driver->ops->call(driver, "raiseEvent", params, &result),
                     NCL_OK);
    NCL_CHECK_EQ_INT(g_events, 1);
    NCL_CHECK_EQ_STR(g_event_id, "alarm:1201");
    ncl_json_free(result);
    ncl_json_free(params);

    NCL_TEST_CASE("mock: the raw escape hatch echoes the frame");
    {
        ncl_driver_result out;
        const uint8_t frame[] = {0xAB, 0xCD};

        NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);
        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->write_raw(driver, frame, sizeof(frame), &out),
                         NCL_OK);
        NCL_CHECK(out.success);
        NCL_CHECK_EQ_INT(out.raw_len, 2);
        NCL_CHECK_EQ_STR(ncl_json_as_string(out.value), "abcd");
        ncl_driver_result_free(&out);

        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, NULL, 0, &out), NCL_OK);
        NCL_CHECK_EQ_INT(out.raw_len, 2);
        NCL_CHECK(out.raw[0] == 0xAB);
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("mock: methods are not supported by every driver");
    {
        ncl_driver *bare = ncl_driver_create("mock");

        NCL_CHECK(bare != NULL);
        NCL_CHECK_EQ_INT(ncl_driver_write_one(bare, NULL, NULL),
                         NCL_ERR_NOT_SUPPORTED);
        bare->ops->destroy(bare);
    }
    driver->ops->destroy(driver);
}

NCL_TEST_MAIN_BEGIN()
    test_types();
    test_errors();
    test_address();
    test_registry();
    test_mock_memory();
    test_mock_failures();
    test_mock_call_and_events();
NCL_TEST_MAIN_END()
