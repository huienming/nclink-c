/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The adapter daemon's core: a driver configuration in, a device out.
 *
 * Everything here runs without a wire - the mock driver stands in for the
 * machine - so the test covers the whole chain: configuration, generated
 * model, the operations the device exposes, and the values they answer with.
 */
#include <stdio.h>

#include "ncl_test.h"

#include "nclink_adapter/ncl_adapter.h"

#define TEST_SN "V2AABBCCDD1"

static const char kConfig[] =
    "{"
    "  \"sn\": \"" TEST_SN "\","
    "  \"drivers\": ["
    "    {"
    "      \"id\": \"plc1\","
    "      \"path\": \"/PLC1\","
    "      \"type\": \"mock\","
    "      \"parameters\": { \"points\": ["
    "        {\"area\": \"D\", \"offset\": 100, \"dtype\": \"int16\","
    "         \"value\": 42},"
    "        {\"area\": \"D\", \"offset\": 200, \"dtype\": \"float32\","
    "         \"value\": 1.5}]},"
    "      \"points\": ["
    "        {\"path\": \"/PLC1/STATUS\", \"addr\": \"D100\"},"
    "        {\"path\": \"/PLC1/POWER\", \"addr\": \"D200\","
    "         \"dtype\": \"float32\", \"writable\": true},"
    "        {\"path\": \"/PLC1/RAW\", \"addr\": \"D300\", \"sample\": false}"
    "      ]"
    "    }"
    "  ],"
    "  \"methods\": ["
    "    {\"path\": \"/PLC1/START\", \"operation\": \"echo\"}"
    "  ],"
    "  \"sample\": { \"intervalMs\": 500, \"uploadMs\": 500 }"
    "}";

static ncl_adapter *make_adapter(void)
{
    ncl_json *config = ncl_json_parse_cstr(kConfig, NULL);
    ncl_strbuf err;
    ncl_adapter *adapter;

    if (config == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&err);
    adapter = ncl_adapter_create(config, &err);
    if (adapter == NULL) {
        printf("    adapter error: %s\n", ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    ncl_json_free(config);
    return adapter;
}

/* ------------------------------------------------------------- requests --- */

static long long query_value(ncl_server *server, const char *path, ncl_err *err)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);
    ncl_message *response;
    long long number = -1;

    *err = NCL_ERR;
    if (request == NULL || item == NULL) {
        ncl_message_free(request);
        return -1;
    }
    ncl_message_set_message_id(request, "q1");
    ncl_params_set_string(&item->params, "operation", "get_value");
    ncl_message_add_query_request_item(request, item);
    response = ncl_server_invoke_query(server, request);
    ncl_message_free(request);
    if (response == NULL) {
        return -1;
    }
    {
        const ncl_query_response_item *result =
            (const ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (result != NULL && result->code != NULL &&
            strcmp(result->code, "OK") == 0 && ncl_message_item_count(response) > 0) {
            ncl_json *values = ncl_json_arr_get(result->values, 0);

            if (values != NULL && ncl_json_as_int(values, &number)) {
                *err = NCL_OK;
            } else {
                double real = 0;

                if (values != NULL && ncl_json_as_double(values, &real)) {
                    number = (long long)real;
                    *err = NCL_OK;
                }
            }
        }
    }
    ncl_message_free(response);
    return number;
}

static ncl_err set_value(ncl_server *server, const char *path, ncl_json *value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);
    ncl_message *response;
    ncl_err result = NCL_ERR;

    ncl_message_set_message_id(request, "s1");
    ncl_params_set_string(&item->params, "operation", "set_value");
    ncl_params_set(&item->params, "value", value);
    ncl_message_add_set_request_item(request, item);
    response = ncl_server_invoke_set(server, request);
    ncl_message_free(request);
    if (response != NULL) {
        const ncl_set_response_item *answer =
            (const ncl_set_response_item *)ncl_message_item_at(response, 0);

        if (answer != NULL && answer->code != NULL &&
            strcmp(answer->code, "OK") == 0) {
            result = NCL_OK;
        }
        ncl_message_free(response);
    }
    return result;
}

/* ---------------------------------------------------------------- tests --- */

static void test_bring_up(void)
{
    ncl_adapter *adapter = make_adapter();
    ncl_server *server;

    NCL_TEST_CASE("the configuration brings up a device");
    NCL_CHECK(adapter != NULL);
    if (adapter == NULL) {
        return;
    }
    NCL_CHECK_EQ_STR(ncl_adapter_sn(adapter), TEST_SN);
    NCL_CHECK_EQ_INT(ncl_adapter_point_count(adapter), 3);
    NCL_CHECK_EQ_INT(ncl_adapter_method_count(adapter), 1);
    NCL_CHECK_EQ_STR(ncl_adapter_point_path(adapter, 0), "/PLC1/STATUS");
    NCL_CHECK(ncl_adapter_point_path(adapter, 7) == NULL);

    server = ncl_adapter_server(adapter);
    NCL_CHECK(server != NULL);
    NCL_CHECK_EQ_STR(ncl_server_sn(server), TEST_SN);

    NCL_TEST_CASE("the generated model carries the point paths");
    {
        ncl_node_map map;

        ncl_node_map_init(&map);
        NCL_CHECK_EQ_INT(ncl_root_node_path_map(ncl_server_model(server), &map),
                         NCL_OK);
        NCL_CHECK(ncl_node_map_get(&map, "/PLC1/STATUS") != NULL);
        NCL_CHECK(ncl_node_map_get(&map, "/PLC1/POWER") != NULL);
        NCL_CHECK(ncl_node_map_get(&map, "/PLC1/RAW") != NULL);
        ncl_node_map_free(&map);
    }

    NCL_TEST_CASE("every point is a queryable operation");
    NCL_CHECK(ncl_server_binding_count(server) >= 4); /* 3 reads + 1 write + call */

    NCL_TEST_CASE("a query answers with the device's value");
    {
        ncl_err err = NCL_ERR;
        long long number = query_value(server, "/PLC1/STATUS", &err);

        NCL_CHECK_EQ_INT(err, NCL_OK);
        NCL_CHECK_EQ_INT(number, 42);
        number = query_value(server, "/PLC1/POWER", &err);
        NCL_CHECK_EQ_INT(err, NCL_OK);
        NCL_CHECK_EQ_INT(number, 1);
        number = query_value(server, "/PLC1/NOPE", &err);
        NCL_CHECK(err != NCL_OK);
    }

    ncl_adapter_free(adapter);
}

static void test_points(void)
{
    ncl_adapter *adapter = make_adapter();
    ncl_server *server = ncl_adapter_server(adapter);
    ncl_err err = NCL_ERR;
    ncl_strbuf poll_err;

    NCL_TEST_CASE("writes go to the device only where the configuration allows");
    NCL_CHECK(adapter != NULL);
    if (adapter == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(set_value(server, "/PLC1/POWER", ncl_json_new_double(2.0)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(query_value(server, "/PLC1/POWER", &err), 2);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    /* /PLC1/STATUS was not opened for writing: no set binding exists, so the
     * write is refused and the device keeps its value. */
    NCL_CHECK(set_value(server, "/PLC1/STATUS", ncl_json_new_int(1)) != NCL_OK);
    NCL_CHECK_EQ_INT(query_value(server, "/PLC1/STATUS", &err), 42);
    NCL_CHECK_EQ_INT(err, NCL_OK);

    NCL_TEST_CASE("polling refreshes the model's values");
    ncl_strbuf_init(&poll_err);
    NCL_CHECK_EQ_INT(ncl_adapter_poll(adapter, &poll_err), NCL_OK);
    NCL_CHECK_EQ_INT(poll_err.len, 0);
    ncl_strbuf_free(&poll_err);
    {
        ncl_node_map map;
        ncl_node *node;

        ncl_node_map_init(&map);
        NCL_CHECK_EQ_INT(ncl_root_node_path_map(ncl_server_model(server), &map),
                         NCL_OK);
        node = ncl_node_map_get(&map, "/PLC1/STATUS");
        NCL_CHECK(node != NULL);
        if (node != NULL) {
            long long value = 0;

            NCL_CHECK(node->value != NULL &&
                      ncl_json_as_int(node->value, &value));
            NCL_CHECK_EQ_INT(value, 42);
        }
        ncl_node_map_free(&map);
    }

    NCL_TEST_CASE("a method call reaches the driver");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response;
        ncl_json *params = ncl_json_new_object();

        ncl_message_set_message_id(request, "m1");
        (void)ncl_json_obj_set_int(params, "spindle", 1200);
        request->as.method_call_request.method = ncl_strdup("echo");
        request->as.method_call_request.params = params;
        response = ncl_server_invoke_method_call(server, request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK(response->as.method_call_response.code != NULL &&
                      strcmp(response->as.method_call_response.code, "OK") == 0);
            ncl_message_free(response);
        }
    }

    ncl_adapter_free(adapter);
}

static void test_bad_config(void)
{
    ncl_strbuf err;
    ncl_adapter *adapter;

    NCL_TEST_CASE("a configuration without a driver is refused");
    ncl_strbuf_init(&err);
    adapter = ncl_adapter_create(
        ncl_json_parse_cstr("{\"sn\":\"V2000000001\",\"drivers\":[]}", NULL),
        &err);
    NCL_CHECK(adapter == NULL);
    NCL_CHECK(err.len > 0);
    ncl_strbuf_free(&err);

    NCL_TEST_CASE("a missing method operation is refused");
    ncl_strbuf_init(&err);
    adapter = ncl_adapter_create(
        ncl_json_parse_cstr(
            "{\"sn\":\"V2000000001\",\"drivers\":[{\"id\":\"m\","
            "\"type\":\"mock\"}],\"methods\":[{\"path\":\"/X\"}]}",
            NULL),
        &err);
    NCL_CHECK(adapter == NULL);
    NCL_CHECK(err.len > 0);
    ncl_strbuf_free(&err);
}

NCL_TEST_MAIN_BEGIN()
    test_bring_up();
    test_points();
    test_bad_config();
NCL_TEST_MAIN_END()
