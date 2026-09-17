/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * End-to-end test of the NC-Link server: requests arrive over MQTT, are
 * dispatched to a registered tool through the model path bindings, and the
 * response is published back on the matching "<...>Response/<sn>" topic.
 */
#include "ncl_test.h"

#include <stdlib.h>

#include "fake_nclink_server.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_topic.h"

#define TEST_SN "V203243111F"

/* The device model used by the tests. The STATUS data item sits directly under
 * the device, so its path is "/STATUS" (see the path rules in MANUAL.md 4.3). */
static const char *kModelJson =
    "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
    "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[],"
    "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"},"
    "{\"id\":\"030002\",\"type\":\"PART_COUNT\"}],\"version\":\"2.0\"}]}";

/* --------------------------------------------------------------- test tool */

typedef struct {
    ncl_json *status;     /* value behind /STATUS */
    int       set_calls;
} test_tool;

static ncl_err tool_get_value(void *instance, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    test_tool *tool = (test_tool *)instance;
    (void)params;
    (void)reason;
    *result = tool->status != NULL ? ncl_json_clone(tool->status)
                                   : ncl_json_new_null();
    return NCL_OK;
}

static ncl_err tool_set_value(void *instance, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    test_tool *tool = (test_tool *)instance;
    ncl_json *value = ncl_json_obj_get(params, "value");

    (void)reason;
    if (value == NULL) {
        return NCL_ERR_INVALID_VALUE;
    }
    ncl_json_free(tool->status);
    tool->status = ncl_json_clone(value);
    tool->set_calls++;
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

static ncl_err tool_get_length(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    (void)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(3);
    return NCL_OK;
}

static ncl_err tool_ping_method(void *instance, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    (void)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_string("pong");
    return NCL_OK;
}

/* --------------------------------------------------------------- MQTT glue */

typedef struct {
    ncl_server *server;
} server_link;

static void on_server_message(void *user, const ncl_mqtt_publish *publish)
{
    server_link *link = (server_link *)user;
    char topic[512];
    ncl_message *message;

    if (publish->topic == NULL) {
        return;
    }
    snprintf(topic, sizeof(topic), "%s", publish->topic);
    message = ncl_message_parse(topic, (const char *)publish->payload,
                                publish->payload_len);
    if (message != NULL) {
        ncl_server_on_message(link->server, topic, message);
    }
}

/* --------------------------------------------------------------- utilities */

/** Wait until the broker has seen one more publish, then parse it. */
static ncl_message *wait_for_response(ncl_fake_server *broker, size_t baseline,
                                      unsigned timeout_ms)
{
    int64_t deadline = ncl_time_monotonic_millis() + (int64_t)timeout_ms;
    char topic[512];
    char payload[2048];

    while (ncl_time_monotonic_millis() < deadline) {
        size_t sequence = ncl_fake_server_last_publish(
            broker, topic, sizeof(topic), payload, sizeof(payload));
        if (sequence > baseline) {
            return ncl_message_parse(topic, payload, strlen(payload));
        }
        ncl_sleep_millis(10);
    }
    return NULL;
}

/** Publish a request message from the "device side". */
static ncl_err send_request(ncl_fake_server *broker, const char *topic,
                            ncl_message *request)
{
    char *payload = ncl_message_write_string(request);
    ncl_err rc;

    ncl_message_free(request);
    if (payload == NULL) {
        return NCL_ERR_NOMEM;
    }
    rc = ncl_fake_server_publish(broker, topic, payload, 2);
    free(payload);
    return rc;
}

static size_t request_baseline(ncl_fake_server *broker)
{
    return ncl_fake_server_last_publish(broker, NULL, 0, NULL, 0);
}

/* ==================================================================== test */

static void test_server_end_to_end(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *broker;
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt;
    server_link link;
    ncl_server_options server_options;
    ncl_server *server;
    test_tool tool;
    char url[128];
    char topic[256];
    static const ncl_tool_method methods[] = {
    {"getValue", tool_get_value, NULL},
    {"setValue", tool_set_value, NULL},
    {"getLength", tool_get_length, NULL},
    {"ping", tool_ping_method, NULL},
    };
    static const ncl_tool_binding bindings[] = {
        {"/STATUS", NCL_OP_GET_VALUE, "getValue", NULL},
        {"/STATUS", NCL_OP_SET_VALUE, "setValue", NULL},
        {"/PART_COUNT", NCL_OP_GET_LENGTH, "getLength", NULL},
    };

    memset(&tool, 0, sizeof(tool));
    tool.status = ncl_json_new_int(42);

    memset(&fake_options, 0, sizeof(fake_options));
    broker = ncl_fake_server_start(&fake_options);
    NCL_CHECK(broker != NULL);
    if (broker == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(broker));

    /* ---- server side: MQTT client + ncl_server ---- */
    memset(&link, 0, sizeof(link));
    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = url;
    mqtt_options.client_id = "ncl-server-test";
    mqtt_options.keep_alive_seconds = 30;
    mqtt_options.automatic_reconnect = false;
    mqtt_options.on_message = on_server_message;
    mqtt_options.user = &link;
    mqtt = ncl_mqtt_client_create(&mqtt_options);
    NCL_CHECK(mqtt != NULL);
    if (mqtt == NULL) {
        return;
    }

    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = TEST_SN;
    server_options.mqtt = mqtt;
    server_options.model_json = kModelJson;
    server = ncl_server_create(&server_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    link.server = server;

    NCL_TEST_CASE("model is loaded and post-constructed");
    {
        ncl_node *root = ncl_server_model(server);
        ncl_node *device;
        NCL_CHECK(root != NULL);
        NCL_CHECK(ncl_node_is_valid(root));
        device = ncl_node_device_at(root, 0);
        NCL_CHECK_EQ_STR(ncl_node_path(device), "/NC_LINK_ROOT/PLC");
        NCL_CHECK_EQ_STR(ncl_node_path(ncl_node_data_item_at(device, 0)), "/STATUS");
    }

    NCL_TEST_CASE("tool methods and path bindings register");
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "testTool", &tool, methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              bindings,
                                              sizeof(bindings) / sizeof(bindings[0])),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_register_builtin_tool(server), NCL_OK);
    /* 4 methods + 3 path bindings + 2 built in methods */
    NCL_CHECK_EQ_INT(ncl_server_binding_count(server), 9);

    NCL_TEST_CASE("server connects and subscribes to its request topics");
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(mqtt), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_subscribe(server), NCL_OK);

    /* ---- query: value ---- */
    NCL_TEST_CASE("QueryRequest is answered with the tool value");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item = ncl_query_request_item_new("/STATUS");
        ncl_message *response;
        size_t baseline = request_baseline(broker);

        ncl_message_set_message_id(request, "q1");
        ncl_params_set_string(&item->params, "operation", "get_value");
        ncl_message_add_query_request_item(request, item);
        snprintf(topic, sizeof(topic), "Query/Request/%s", TEST_SN);
        NCL_CHECK_EQ_INT(send_request(broker, topic, request), NCL_OK);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *result =
                (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK_EQ_INT(response->type, NCL_MSG_QUERY_RESPONSE);
            NCL_CHECK_EQ_STR(response->message_id, "q1");
            NCL_CHECK(result != NULL);
            if (result != NULL) {
                long long value = 0;
                NCL_CHECK_EQ_STR(result->code, NCL_KW_CODE_OK);
                NCL_CHECK(ncl_json_as_int(ncl_query_response_item_data(result),
                                          &value));
                NCL_CHECK_EQ_INT(value, 42);
            }
            ncl_message_free(response);
        }
    }

    /* ---- query by node id (no leading slash) ---- */
    NCL_TEST_CASE("a request addressed by node id resolves through the model");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item = ncl_query_request_item_new("030001");
        ncl_message *response;
        size_t baseline = request_baseline(broker);

        ncl_message_set_message_id(request, "q2");
        ncl_params_set_string(&item->params, "operation", "get_value");
        ncl_message_add_query_request_item(request, item);
        snprintf(topic, sizeof(topic), "Query/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *result =
                (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(result != NULL && result->code != NULL);
            NCL_CHECK_EQ_STR(result->code, NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
    }

    /* ---- query: unknown path ---- */
    NCL_TEST_CASE("an unbound path answers code NG");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item = ncl_query_request_item_new("/NOPE");
        ncl_message *response;
        size_t baseline = request_baseline(broker);

        ncl_message_set_message_id(request, "q3");
        ncl_message_add_query_request_item(request, item);
        snprintf(topic, sizeof(topic), "Query/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *result =
                (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(result != NULL && result->code != NULL);
            NCL_CHECK_EQ_STR(result->code, NCL_KW_CODE_NG);
            ncl_message_free(response);
        }
    }

    /* ---- get_length ---- */
    NCL_TEST_CASE("get_length reaches the bound method");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item = ncl_query_request_item_new("/PART_COUNT");
        ncl_message *response;
        size_t baseline = request_baseline(broker);

        ncl_message_set_message_id(request, "q4");
        ncl_params_set_string(&item->params, "operation", "get_length");
        ncl_message_add_query_request_item(request, item);
        snprintf(topic, sizeof(topic), "Query/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *result =
                (ncl_query_response_item *)ncl_message_item_at(response, 0);
            long long length = 0;
            NCL_CHECK(result != NULL);
            NCL_CHECK(ncl_json_as_int(ncl_query_response_item_data(result), &length));
            NCL_CHECK_EQ_INT(length, 3);
            ncl_message_free(response);
        }
    }

    /* ---- set ---- */
    NCL_TEST_CASE("SetRequest updates the tool state");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
        ncl_set_request_item *item = ncl_set_request_item_new("/STATUS");
        ncl_message *response;
        size_t baseline = request_baseline(broker);

        ncl_message_set_message_id(request, "s1");
        ncl_params_set_string(&item->params, "operation", "set_value");
        ncl_params_set(&item->params, "value", ncl_json_new_int(99));
        ncl_message_add_set_request_item(request, item);
        snprintf(topic, sizeof(topic), "Set/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_set_response_item *result =
                (ncl_set_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK_EQ_INT(response->type, NCL_MSG_SET_RESPONSE);
            NCL_CHECK(result != NULL);
            NCL_CHECK_EQ_STR(result->code, NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
        NCL_CHECK_EQ_INT(tool.set_calls, 1);
        {
            long long stored = 0;
            NCL_CHECK(ncl_json_as_int(tool.status, &stored));
            NCL_CHECK_EQ_INT(stored, 99);
        }
    }

    /* ---- probe ---- */
    NCL_TEST_CASE("ProbeQueryRequest returns the device model");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_PROBE_QUERY_REQUEST);
        ncl_message *response;
        size_t baseline = request_baseline(broker);

        ncl_message_set_message_id(request, "p1");
        snprintf(topic, sizeof(topic), "Probe/Query/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_INT(response->type, NCL_MSG_PROBE_QUERY_RESPONSE);
            NCL_CHECK_EQ_STR(response->as.probe_query_response.code, NCL_KW_CODE_OK);
            NCL_CHECK(response->as.probe_query_response.model != NULL);
            NCL_CHECK(ncl_node_is_valid(response->as.probe_query_response.model));
            ncl_message_free(response);
        }
    }

    /* ---- ping ---- */
    NCL_TEST_CASE("Ping is answered with a Pong on the Pong topic");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_PING);
        ncl_message *response;
        size_t baseline = request_baseline(broker);
        char expected_topic[64];

        ncl_message_set_message_id(request, "ping1");
        snprintf(topic, sizeof(topic), "Ping/%s", TEST_SN);
        snprintf(expected_topic, sizeof(expected_topic), "Pong/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        NCL_CHECK_EQ_STR(ncl_fake_server_last_topic(broker), expected_topic);
        if (response != NULL) {
            NCL_CHECK_EQ_INT(response->type, NCL_MSG_PONG);
            NCL_CHECK_EQ_STR(response->message_id, "ping1");
            ncl_message_free(response);
        }
    }

    /* ---- method call ---- */
    NCL_TEST_CASE("MethodCallRequest reaches a registered tool method");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response;
        size_t baseline = request_baseline(broker);
        ncl_json *params = ncl_json_new_object();

        ncl_message_set_message_id(request, "m1");
        ncl_message_set_method(request, "/testTool/ping");
        ncl_json_obj_set_string(params, "x", "1");
        ncl_message_set_params(request, params);
        snprintf(topic, sizeof(topic), "Method/Call/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_INT(response->type, NCL_MSG_METHOD_CALL_RESPONSE);
            NCL_CHECK_EQ_STR(response->as.method_call_response.code, NCL_KW_CODE_OK);
            NCL_CHECK(response->as.method_call_response.data != NULL);
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("addSample / removeSample through the built in tool");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response;
        ncl_json *params = ncl_json_new_object();
        ncl_json *config = ncl_json_new_object();
        ncl_json *ids = ncl_json_new_array();
        ncl_json *ref = ncl_json_new_object();
        size_t baseline;

        /* A complete SAMPLE_CHANNEL: a long upload interval keeps the sampler
         * quiet for the duration of this test. */
        ncl_json_obj_set_string(config, "id", "ch1");
        ncl_json_obj_set_string(config, "type", "SAMPLE_CHANNEL");
        ncl_json_obj_set_int(config, "sampleInterval", 1000);
        ncl_json_obj_set_int(config, "uploadInterval", 60000);
        ncl_json_obj_set_string(ref, "id", "/STATUS");
        ncl_json_arr_push(ids, ref);
        ncl_json_obj_set(config, "ids", ids);
        ncl_json_obj_set(params, "request", config);
        ncl_message_set_message_id(request, "m2");
        ncl_message_set_method(request, "/nclinkServer/addSample");
        ncl_message_set_params(request, params);
        baseline = request_baseline(broker);
        snprintf(topic, sizeof(topic), "Method/Call/Request/%s", TEST_SN);
        send_request(broker, topic, request);

        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_STR(response->as.method_call_response.code, NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
        NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 1);

        /* remove */
        request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        params = ncl_json_new_object();
        ncl_json_obj_set_string(params, "id", "ch1");
        ncl_message_set_message_id(request, "m3");
        ncl_message_set_method(request, "/nclinkServer/removeSample");
        ncl_message_set_params(request, params);
        baseline = request_baseline(broker);
        send_request(broker, topic, request);
        response = wait_for_response(broker, baseline, 5000);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_STR(response->as.method_call_response.code, NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
        NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 0);
    }

    ncl_server_free(server);
    ncl_mqtt_client_destroy(mqtt);
    ncl_fake_server_stop(broker);
    ncl_json_free(tool.status);
}

/* =========================================================== sampling test */

/* A model with one SAMPLE_CHANNEL whose two sample items are addressed by path
 * ("/STATUS") and by node id ("030002"). */
static const char *kSampleModelJson =
    "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
    "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\","
    "\"configs\":[{\"id\":\"ch1\",\"type\":\"SAMPLE_CHANNEL\","
    "\"sampleInterval\":40,\"uploadInterval\":120,"
    "\"ids\":[{\"id\":\"/STATUS\"},{\"id\":\"030002\"}]}],"
    "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"},"
    "{\"id\":\"030002\",\"type\":\"PART_COUNT\"}],\"version\":\"2.0\"}]}";

typedef struct {
    int status_calls;
    int part_count_calls;
    int ext_calls;
    int trace_calls;
    int jitter_calls;
} sample_tool;

static ncl_err sample_get_status(void *instance, const ncl_json *params,
                                 ncl_json **result, char **reason)
{
    sample_tool *tool = (sample_tool *)instance;
    (void)params;
    (void)reason;
    tool->status_calls++;
    *result = ncl_json_new_int(42);
    return NCL_OK;
}

static ncl_err sample_get_part_count(void *instance, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    sample_tool *tool = (sample_tool *)instance;
    (void)params;
    (void)reason;
    tool->part_count_calls++;
    *result = ncl_json_new_int(7);
    return NCL_OK;
}

/** 一个只存在于绑定里、模型里没有的路径（表头直接给路径的场景）。 */
static ncl_err sample_get_ext(void *instance, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    sample_tool *tool = (sample_tool *)instance;
    (void)params;
    (void)reason;
    tool->ext_calls++;
    *result = ncl_json_new_int(11);
    return NCL_OK;
}

/**
 * 亚毫秒采样的数据源：一次调用返回一批值（例如 1ms 槽位里采到的 10 个点）。
 * 库不对返回值做任何假设，"值本身是数组"就直接成为列里的一个内层数组。
 */
static ncl_err sample_get_trace(void *instance, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    sample_tool *tool = (sample_tool *)instance;
    ncl_json *batch = ncl_json_new_array();
    int i;

    (void)params;
    (void)reason;
    tool->trace_calls++;
    for (i = 0; i < 10; i++) {
        ncl_json_arr_push(batch, ncl_json_new_int(tool->trace_calls * 100 + i));
    }
    *result = batch;
    return NCL_OK;
}

/** 批次长度会抖动的数据源：第一槽 10 个点、之后 9 个 —— 内层不对齐。 */
static ncl_err sample_get_jitter(void *instance, const ncl_json *params,
                                 ncl_json **result, char **reason)
{
    sample_tool *tool = (sample_tool *)instance;
    ncl_json *batch = ncl_json_new_array();
    int count;
    int i;

    (void)params;
    (void)reason;
    tool->jitter_calls++;
    count = tool->jitter_calls == 1 ? 10 : 9;
    for (i = 0; i < count; i++) {
        ncl_json_arr_push(batch, ncl_json_new_int(i));
    }
    *result = batch;
    return NCL_OK;
}

static ncl_node *config_from_json(const char *json)
{
    ncl_json *document = ncl_json_parse_cstr(json, NULL);
    ncl_node *config = NULL;

    if (document != NULL) {
        config = ncl_node_from_json(document, NCL_NODE_CONFIG);
        ncl_json_free(document);
    }
    return config;
}

/*
 * 采样通道的三种情形：
 *   情况 1  模型文件里有定义（见 test_sampling）
 *   情况 2  模型里没有定义，但采样项直接给了路径（表头）
 *   其他    一律当异常：返回具体错误码，且不启动任务
 * 无论哪种，消费端拿到的一定是"表头 + 数据块"完整对齐的报文。
 */
static void test_sample_channel_shapes(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *broker;
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt;
    server_link link;
    ncl_server_options server_options;
    ncl_server *server;
    sample_tool tool;
    char url[128];
    char topic[512];
    char payload[4096];
    int i;
    /* 模型里只有 /STATUS，没有任何 SAMPLE_CHANNEL，也没有 /EXT/A@0 */
    static const char *kModelJson =
        "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
        "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[],"
        "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}],"
        "\"version\":\"2.0\"}]}";
    static const ncl_tool_method methods[] = {
        {"getValue", sample_get_status, NULL},
        {"getExt", sample_get_ext, NULL},
    };
    static const ncl_tool_binding bindings[] = {
        {"/STATUS", NCL_OP_GET_VALUE, "getValue", NULL},
        {"/EXT/A@0", NCL_OP_GET_VALUE, "getExt", NULL},
    };

    memset(&tool, 0, sizeof(tool));
    memset(&fake_options, 0, sizeof(fake_options));
    broker = ncl_fake_server_start(&fake_options);
    NCL_CHECK(broker != NULL);
    if (broker == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(broker));
    memset(&link, 0, sizeof(link));
    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = url;
    mqtt_options.client_id = "ncl-sample-shapes";
    mqtt_options.keep_alive_seconds = 30;
    mqtt_options.automatic_reconnect = false;
    mqtt_options.on_message = on_server_message;
    mqtt_options.user = &link;
    mqtt = ncl_mqtt_client_create(&mqtt_options);
    NCL_CHECK(mqtt != NULL);
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = TEST_SN;
    server_options.mqtt = mqtt;
    server_options.model_json = kModelJson;
    server = ncl_server_create(&server_options);
    NCL_CHECK(server != NULL);
    if (server == NULL || mqtt == NULL) {
        ncl_fake_server_stop(broker);
        return;
    }
    link.server = server;
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "plant", &tool, methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              bindings,
                                              sizeof(bindings) / sizeof(bindings[0])),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(mqtt), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_subscribe(server), NCL_OK);

    NCL_TEST_CASE("情况 2：模型里没有采样通道，但采样项直接给了路径（表头）");
    {
        ncl_node *config = config_from_json(
            "{\"id\":\"chExt\",\"type\":\"SAMPLE_CHANNEL\","
            "\"sampleInterval\":40,\"uploadInterval\":80,"
            "\"ids\":[{\"id\":\"/EXT/A@0\"},{\"id\":\"/STATUS\"}]}");
        NCL_CHECK(config != NULL);
        if (config != NULL) {
            NCL_CHECK_EQ_INT(ncl_server_add_sample(server, config), NCL_OK);
            NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 1);
            ncl_node_free(config);
        }
        for (i = 0; i < 150 && ncl_server_sample_upload_count(server) < 1; i++) {
            ncl_sleep_millis(20);
        }
        NCL_CHECK(ncl_server_sample_upload_count(server) >= 1);
        NCL_CHECK(tool.ext_calls >= 1);      /* 表头里的路径真的被采了 */
        NCL_CHECK(tool.status_calls >= 1);

        NCL_CHECK(ncl_fake_server_publish_at(broker, 0, topic, sizeof(topic),
                                             payload, sizeof(payload)));
        NCL_CHECK_EQ_STR(topic, "Sample/" TEST_SN "/chExt");
        NCL_CHECK(strstr(payload, "\"paths\":[\"/EXT/A@0\",\"/STATUS\"]") != NULL);

        /* 消费端拿到的是完整报文：表头项数 == 数据块列数，各列取值个数一致 */
        {
            ncl_message *sample = ncl_message_parse(topic, payload, strlen(payload));
            NCL_CHECK(sample != NULL);
            if (sample != NULL) {
                char *header = ncl_message_sample_header(sample, ";");
                NCL_CHECK_EQ_STR(header, "/EXT/A@0;/STATUS");
                free(header);
                NCL_CHECK(ncl_message_sample_is_complete(sample));
                NCL_CHECK_EQ_INT(ncl_message_item_count(sample), 2);
                NCL_CHECK_EQ_INT(ncl_strvec_len(&sample->as.sample.paths), 2);
                NCL_CHECK_EQ_STR(sample->as.sample.id, "chExt");
                NCL_CHECK_EQ_INT(sample->as.sample.interval, 40);
                NCL_CHECK_EQ_INT(sample->as.sample.upload_interval, 80);
                ncl_message_free(sample);
            }
        }
        NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "chExt"), NCL_OK);
    }

    NCL_TEST_CASE("其他情况都按异常处理，不启动任务");
    {
        struct {
            const char *json;
            ncl_err     expected;
            const char *what;
        } cases[] = {
            /* 既不是路径，模型里也找不到 */
            {"{\"id\":\"bad1\",\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":40,"
             "\"uploadInterval\":80,\"ids\":[{\"id\":\"030999\"}]}",
             NCL_ERR_NOT_FOUND, "无法解析的节点 id"},
            /* 没有采样项 */
            {"{\"id\":\"bad2\",\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":40,"
             "\"uploadInterval\":80}",
             NCL_ERR_INVALID_MODEL, "ids 为空"},
            /* 缺 sampleInterval */
            {"{\"id\":\"bad3\",\"type\":\"SAMPLE_CHANNEL\",\"uploadInterval\":80,"
             "\"ids\":[{\"id\":\"/STATUS\"}]}",
             NCL_ERR_INVALID_ARG, "缺 sampleInterval"},
            /* 缺 uploadInterval */
            {"{\"id\":\"bad4\",\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":40,"
             "\"ids\":[{\"id\":\"/STATUS\"}]}",
             NCL_ERR_INVALID_ARG, "缺 uploadInterval"},
            /* 周期为 0 */
            {"{\"id\":\"bad5\",\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":0,"
             "\"uploadInterval\":80,\"ids\":[{\"id\":\"/STATUS\"}]}",
             NCL_ERR_INVALID_ARG, "sampleInterval 为 0"},
        };
        size_t c;

        for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            ncl_node *config = config_from_json(cases[c].json);
            NCL_CHECK(config != NULL);
            if (config == NULL) {
                continue;
            }
            if (ncl_server_add_sample(server, config) != cases[c].expected) {
                printf("      [%s] 期望 %s，实际 %s\n", cases[c].what,
                       ncl_err_name(cases[c].expected),
                       ncl_err_name(ncl_server_add_sample(server, config)));
                NCL_CHECK(0);
            }
            ncl_node_free(config);
        }
        NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 0);
    }

    ncl_server_free(server);
    ncl_mqtt_client_destroy(mqtt);
    ncl_fake_server_stop(broker);
}

static void test_sampling(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *broker;
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt;
    server_link link;
    ncl_server_options server_options;
    ncl_server *server;
    sample_tool tool;
    char url[128];
    char topic[512];
    char payload[2048];
    size_t uploads;
    int i;
    static const ncl_tool_method methods[] = {
    {"getValue", sample_get_status, NULL},
    {"getCount", sample_get_part_count, NULL},
    };
    static const ncl_tool_binding bindings[] = {
        {"/STATUS", NCL_OP_GET_VALUE, "getValue", NULL},
        {"/PART_COUNT", NCL_OP_GET_VALUE, "getCount", NULL},
    };

    memset(&tool, 0, sizeof(tool));
    memset(&fake_options, 0, sizeof(fake_options));
    broker = ncl_fake_server_start(&fake_options);
    NCL_CHECK(broker != NULL);
    if (broker == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(broker));

    memset(&link, 0, sizeof(link));
    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = url;
    mqtt_options.client_id = "ncl-sample-test";
    mqtt_options.keep_alive_seconds = 30;
    mqtt_options.automatic_reconnect = false;
    mqtt_options.on_message = on_server_message;
    mqtt_options.user = &link;
    mqtt = ncl_mqtt_client_create(&mqtt_options);
    NCL_CHECK(mqtt != NULL);
    if (mqtt == NULL) {
        return;
    }

    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = TEST_SN;
    server_options.mqtt = mqtt;
    server_options.model_json = kSampleModelJson;
    server = ncl_server_create(&server_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    link.server = server;
    ncl_server_register_tool(server, "sampleTool", &tool, methods,
                             sizeof(methods) / sizeof(methods[0]), bindings,
                             sizeof(bindings) / sizeof(bindings[0]));

    NCL_TEST_CASE("the sample channel survives post-construction");
    {
        ncl_node *device = ncl_node_device_at(ncl_server_model(server), 0);
        ncl_node *config = ncl_node_config_at(device, 0);
        NCL_CHECK(config != NULL);
        NCL_CHECK(ncl_node_is_sample_node(config));
        NCL_CHECK_EQ_INT(ncl_node_sample_count(config), 2);
        NCL_CHECK(config->has_sample_interval);
        NCL_CHECK_EQ_INT(config->sample_interval, 40);
        NCL_CHECK_EQ_INT(config->upload_interval, 120);
    }

    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(mqtt), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_subscribe(server), NCL_OK);

    NCL_TEST_CASE("every SAMPLE_CHANNEL config gets a sampling task");
    NCL_CHECK_EQ_INT(ncl_server_init_samples(server), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 1);

    NCL_TEST_CASE("the sampler publishes Sample messages on Sample/<sn>/<id>");
    for (i = 0; i < 150 && ncl_server_sample_upload_count(server) < 2; i++) {
        ncl_sleep_millis(20);
    }
    uploads = ncl_server_sample_upload_count(server);
    NCL_CHECK(uploads >= 2);
    NCL_CHECK(tool.status_calls >= 2);
    NCL_CHECK(tool.part_count_calls >= 2);

    {
        ncl_message *sample = NULL;
        size_t index;
        for (index = 0; index < 8; index++) {
            if (!ncl_fake_server_publish_at(broker, index, topic, sizeof(topic),
                                            payload, sizeof(payload))) {
                break;
            }
            if (strncmp(topic, "Sample/", 7) == 0) {
                sample = ncl_message_parse(topic, payload, strlen(payload));
                break;
            }
        }
        NCL_CHECK(sample != NULL);
        if (sample != NULL) {
            NCL_CHECK_EQ_INT(sample->type, NCL_MSG_SAMPLE);
            NCL_CHECK_EQ_STR(topic, "Sample/" TEST_SN "/ch1");
            NCL_CHECK_EQ_STR(sample->as.sample.id, "ch1");
            NCL_CHECK_EQ_INT(sample->as.sample.interval, 40);
            NCL_CHECK_EQ_INT(sample->as.sample.upload_interval, 120);
            NCL_CHECK_EQ_INT(ncl_message_item_count(sample), 2);
            NCL_CHECK_EQ_INT(ncl_strvec_len(&sample->as.sample.paths), 2);
            /* 表头：线上必须是数组 "paths":["/STATUS","/PART_COUNT"]，
             * 且顺序与采样项一致。 */
            NCL_CHECK_EQ_STR(ncl_strvec_at(&sample->as.sample.paths, 0),
                             "/STATUS");
            NCL_CHECK_EQ_STR(ncl_strvec_at(&sample->as.sample.paths, 1),
                             "/PART_COUNT");
            NCL_CHECK(strstr(payload, "\"paths\":[\"/STATUS\",\"/PART_COUNT\"]")
                      != NULL);
            /* beginTime 必须是墙上时钟（不是单调时钟），
             * 而不是本进程开机后的单调值。 */
            {
                long long begin = atoll(sample->as.sample.begin_time != NULL
                                            ? sample->as.sample.begin_time
                                            : "0");
                NCL_CHECK(begin > 1600000000000LL);
            }
            {
                char *header = ncl_message_sample_header(sample, ";");
                NCL_CHECK_EQ_STR(header, "/STATUS;/PART_COUNT");
                free(header);
                header = ncl_message_sample_header(sample, NULL);
                NCL_CHECK_EQ_STR(header, "/STATUS;/PART_COUNT");
                free(header);
            }
            {
                const ncl_sample_item *first =
                    (const ncl_sample_item *)ncl_message_item_at(sample, 0);
                const ncl_sample_item *second =
                    (const ncl_sample_item *)ncl_message_item_at(sample, 1);
                long long value = 0;
                NCL_CHECK(first != NULL && second != NULL);
                if (first != NULL && second != NULL) {
                    /* three rounds per upload window (120 / 40) */
                    NCL_CHECK_EQ_INT(ncl_json_arr_len(first->data), 3);
                    NCL_CHECK_EQ_INT(ncl_json_arr_len(second->data), 3);
                    NCL_CHECK(ncl_json_as_int(ncl_json_arr_get(first->data, 0),
                                              &value));
                    NCL_CHECK_EQ_INT(value, 42);
                    NCL_CHECK(ncl_json_as_int(ncl_json_arr_get(second->data, 0),
                                              &value));
                    NCL_CHECK_EQ_INT(value, 7);
                }
            }
            ncl_message_free(sample);
        }
    }

    NCL_TEST_CASE("removeSample stops the task");
    NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "ch1"), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 0);
    {
        size_t after_stop = ncl_server_sample_upload_count(server);
        ncl_sleep_millis(250);
        NCL_CHECK_EQ_INT(ncl_server_sample_upload_count(server), after_stop);
    }
    NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "ch1"), NCL_ERR_NOT_FOUND);

    ncl_server_free(server);
    ncl_mqtt_client_destroy(mqtt);
    ncl_fake_server_stop(broker);
}

/*
 * 亚毫秒采样：采样周期最小仍是 1ms（外层槽位），但**一次采样可以返回一批值**
 * （值本身是数组），于是列里每个槽位是一个内层数组，例如
 *   槽位 1ms、上传 200ms、每次返回 10 个点 → 200 个槽位 × 每槽 10 点 = 2000 点
 * 库不配置、不加字段，批次长度完全由数据（工具返回值）决定；
 * 只保证"同一槽位各列结构一致"——不一致的报文按不完整丢弃。
 */
static void test_sub_millisecond_samples(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *broker;
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt;
    server_link link;
    ncl_server_options server_options;
    ncl_server *server;
    sample_tool tool;
    char url[128];
    char topic[512];
    char payload[8192];
    int i;
    size_t uploads;
    static const char *kModelJson =
        "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
        "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[],"
        "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}],"
        "\"version\":\"2.0\"}]}";
    static const ncl_tool_method methods[] = {
        {"getValue", sample_get_status, NULL},
        {"getTrace", sample_get_trace, NULL},
        {"getJitter", sample_get_jitter, NULL},
    };
    static const ncl_tool_binding bindings[] = {
        {"/STATUS", NCL_OP_GET_VALUE, "getValue", NULL},
        {"/TRACE@0", NCL_OP_GET_VALUE, "getTrace", NULL},
        {"/JITTER@0", NCL_OP_GET_VALUE, "getJitter", NULL},
    };

    memset(&tool, 0, sizeof(tool));
    memset(&fake_options, 0, sizeof(fake_options));
    broker = ncl_fake_server_start(&fake_options);
    NCL_CHECK(broker != NULL);
    if (broker == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(broker));
    memset(&link, 0, sizeof(link));
    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = url;
    mqtt_options.client_id = "ncl-sample-nested";
    mqtt_options.keep_alive_seconds = 30;
    mqtt_options.automatic_reconnect = false;
    mqtt_options.on_message = on_server_message;
    mqtt_options.user = &link;
    mqtt = ncl_mqtt_client_create(&mqtt_options);
    NCL_CHECK(mqtt != NULL);
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = TEST_SN;
    server_options.mqtt = mqtt;
    server_options.model_json = kModelJson;
    server = ncl_server_create(&server_options);
    NCL_CHECK(server != NULL);
    if (server == NULL || mqtt == NULL) {
        ncl_fake_server_stop(broker);
        return;
    }
    link.server = server;
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "plant", &tool, methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              bindings,
                                              sizeof(bindings) / sizeof(bindings[0])),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_mqtt_client_connect(mqtt), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_subscribe(server), NCL_OK);

    NCL_TEST_CASE("工具返回一批值时，列的每个槽位是一个内层数组");
    {
        ncl_node *config = config_from_json(
            "{\"id\":\"chNested\",\"type\":\"SAMPLE_CHANNEL\","
            "\"sampleInterval\":40,\"uploadInterval\":80,"
            "\"ids\":[{\"id\":\"/TRACE@0\"}]}");
        NCL_CHECK(config != NULL);
        if (config != NULL) {
            NCL_CHECK_EQ_INT(ncl_server_add_sample(server, config), NCL_OK);
            ncl_node_free(config);
        }
        for (i = 0; i < 150 && ncl_server_sample_upload_count(server) < 1; i++) {
            ncl_sleep_millis(20);
        }
        NCL_CHECK(ncl_server_sample_upload_count(server) >= 1);
        NCL_CHECK(tool.trace_calls >= 1);

        NCL_CHECK(ncl_fake_server_publish_at(broker, 0, topic, sizeof(topic),
                                             payload, sizeof(payload)));
        /* 外层是槽位数组，内层是每个槽位里的一批值 */
        NCL_CHECK(strstr(payload, "\"paths\":[\"/TRACE@0\"]") != NULL);
        NCL_CHECK(strstr(payload, "\"data\":[[") != NULL);

        {
            ncl_message *sample = ncl_message_parse(topic, payload, strlen(payload));
            NCL_CHECK(sample != NULL);
            if (sample != NULL) {
                const ncl_sample_item *item =
                    (const ncl_sample_item *)ncl_message_item_at(sample, 0);
                /* 80ms / 40ms = 2 个槽位，每槽 10 个点 = 20 点 */
                NCL_CHECK_EQ_INT(ncl_json_arr_len(item->data), 2);
                NCL_CHECK_EQ_INT(
                    ncl_json_arr_len(ncl_json_arr_get(item->data, 0)), 10);
                NCL_CHECK(ncl_sample_item_is_nested(item));
                NCL_CHECK_EQ_INT(ncl_sample_item_value_count(item), 20);
                NCL_CHECK_EQ_INT(ncl_message_sample_point_count(sample), 20);
                /* 扁平下标跨槽位读数：第 11 个点 = 第 2 槽的第 1 个 */
                {
                    long long value = 0;
                    const ncl_json *point = ncl_sample_item_value_at(item, 10);
                    NCL_CHECK(point != NULL);
                    NCL_CHECK(ncl_json_as_int(point, &value));
                    NCL_CHECK_EQ_INT(value % 100, 0); /* 每批的第一个 */
                }
                NCL_CHECK(ncl_message_sample_is_complete(sample));
                ncl_message_free(sample);
            }
        }
        NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "chNested"), NCL_OK);
    }

    NCL_TEST_CASE("采样率不同的一标量列 + 一批量列可以在同一个通道里");
    {
        ncl_node *config = config_from_json(
            "{\"id\":\"chMixed\",\"type\":\"SAMPLE_CHANNEL\","
            "\"sampleInterval\":40,\"uploadInterval\":80,"
            "\"ids\":[{\"id\":\"/STATUS\"},{\"id\":\"/TRACE@0\"}]}");
        int trace_before = tool.trace_calls;
        size_t uploads_before = ncl_server_sample_upload_count(server);

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            NCL_CHECK_EQ_INT(ncl_server_add_sample(server, config), NCL_OK);
            ncl_node_free(config);
        }
        /* 外层槽位一致即可：/STATUS 每槽 1 点、/TRACE@0 每槽 10 点 */
        for (i = 0; i < 150 && ncl_server_sample_upload_count(server) == uploads_before;
             i++) {
            ncl_sleep_millis(20);
        }
        NCL_CHECK(tool.trace_calls > trace_before);
        NCL_CHECK(ncl_server_sample_upload_count(server) > uploads_before);
        NCL_CHECK(ncl_fake_server_publish_at(broker, 0, topic, sizeof(topic),
                                             payload, sizeof(payload)));
        NCL_CHECK(strstr(payload, "\"paths\":[\"/STATUS\",\"/TRACE@0\"]") != NULL);
        {
            ncl_message *sample = ncl_message_parse(topic, payload, strlen(payload));
            NCL_CHECK(sample != NULL);
            if (sample != NULL) {
                const ncl_sample_item *flat =
                    (const ncl_sample_item *)ncl_message_item_at(sample, 0);
                const ncl_sample_item *nested =
                    (const ncl_sample_item *)ncl_message_item_at(sample, 1);
                NCL_CHECK(ncl_message_sample_is_complete(sample));
                NCL_CHECK(flat != NULL && !ncl_sample_item_is_nested(flat));
                NCL_CHECK(nested != NULL && ncl_sample_item_is_nested(nested));
                NCL_CHECK_EQ_INT(ncl_sample_item_value_count(flat), 2);    /* 2 个槽位 */
                NCL_CHECK_EQ_INT(ncl_sample_item_value_count(nested), 20); /* 2×10 点 */
                /* 行数取最多的一列（批量列 20 点） */
                NCL_CHECK_EQ_INT(ncl_message_sample_point_count(sample), 20);
                /* 按行读：批量列逐点展开，标量列每 10 行取同一个点 */
                {
                    long long value = 0;
                    const ncl_json *row0 = ncl_message_sample_value_at(sample, 0, 0);
                    const ncl_json *row9 = ncl_message_sample_value_at(sample, 9, 0);
                    const ncl_json *row10 = ncl_message_sample_value_at(sample, 10, 0);
                    const ncl_json *batch9 = ncl_message_sample_value_at(sample, 9, 1);
                    const ncl_json *batch10 = ncl_message_sample_value_at(sample, 10, 1);

                    NCL_CHECK(row0 != NULL && ncl_json_as_int(row0, &value));
                    NCL_CHECK(row9 != NULL && ncl_json_as_int(row9, &value));
                    NCL_CHECK(row10 != NULL && ncl_json_as_int(row10, &value));
                    /* 标量列只有 2 个点（2 个槽位）：第 0~9 行都取第 1 个点（42） */
                    NCL_CHECK_EQ_INT(value, 42);
                    NCL_CHECK(ncl_json_as_int(row0, &value) && value == 42);
                    NCL_CHECK(ncl_json_as_int(row9, &value) && value == 42);
                    /* 批量列就是行轴，逐点展开：第 9 行 = 第 1 槽最后一点，
                     * 第 10 行 = 第 2 槽的第一点（每批的第一个点 % 100 == 0） */
                    NCL_CHECK(batch9 != NULL && ncl_json_as_int(batch9, &value) &&
                              value % 100 == 9);
                    NCL_CHECK(batch10 != NULL && ncl_json_as_int(batch10, &value) &&
                              value % 100 == 0);
                }
                ncl_message_free(sample);
            }
        }
        NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "chMixed"), NCL_OK);
    }

    NCL_TEST_CASE("老路径不受影响：标量列仍是扁平数组");
    {
        ncl_node *config = config_from_json(
            "{\"id\":\"chFlat\",\"type\":\"SAMPLE_CHANNEL\","
            "\"sampleInterval\":40,\"uploadInterval\":80,"
            "\"ids\":[{\"id\":\"/STATUS\"}]}");
        NCL_CHECK(config != NULL);
        if (config != NULL) {
            NCL_CHECK_EQ_INT(ncl_server_add_sample(server, config), NCL_OK);
            ncl_node_free(config);
        }
        uploads = ncl_server_sample_upload_count(server);
        for (i = 0; i < 150 && ncl_server_sample_upload_count(server) == uploads; i++) {
            ncl_sleep_millis(20);
        }
        NCL_CHECK(ncl_server_sample_upload_count(server) > uploads);
        NCL_CHECK(ncl_fake_server_publish_at(broker, 0, topic, sizeof(topic),
                                             payload, sizeof(payload)));
        NCL_CHECK(strstr(payload, "\"data\":[[") == NULL); /* 没有嵌套 */
        {
            ncl_message *sample = ncl_message_parse(topic, payload, strlen(payload));
            NCL_CHECK(sample != NULL);
            if (sample != NULL) {
                const ncl_sample_item *item =
                    (const ncl_sample_item *)ncl_message_item_at(sample, 0);
                NCL_CHECK(!ncl_sample_item_is_nested(item));
                NCL_CHECK_EQ_INT(ncl_sample_item_value_count(item), 2);
                NCL_CHECK(ncl_message_sample_is_complete(sample));
                ncl_message_free(sample);
            }
        }
        NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "chFlat"), NCL_OK);
    }

    NCL_TEST_CASE("完整性校验：外层槽位对齐即可，内层长度不强制一致");
    {
        ncl_message *msg = ncl_message_new(NCL_MSG_SAMPLE);
        ncl_sample_item *item = ncl_sample_item_new();
        ncl_json *batch;
        int i;

        ncl_message_set_sample_id(msg, "chUnit");
        ncl_message_set_sample_interval(msg, 1);
        ncl_message_set_upload_interval(msg, 2);
        ncl_message_add_sample_path(msg, "/TRACE@0");
        for (i = 0; i < 2; i++) { /* 两个槽位，每个 10 点 */
            batch = ncl_json_new_array();
            ncl_json_arr_push(batch, ncl_json_new_int(i * 10));
            ncl_json_arr_push(batch, ncl_json_new_int(0));
            while (ncl_json_arr_len(batch) < 10) {
                ncl_json_arr_push(batch, ncl_json_new_int(1));
            }
            ncl_sample_item_add_value(item, batch);
        }
        ncl_message_add_sample_item(msg, item);
        NCL_CHECK(ncl_message_sample_is_complete(msg));
        NCL_CHECK_EQ_INT(ncl_message_sample_point_count(msg), 20);

        /* 把第二个槽位换成 9 个点 → 内层不齐，但外层（2 个槽位）仍对齐 → 依然完整 */
        ncl_json_free(ncl_json_arr_take(item->data, 1));
        batch = ncl_json_new_array();
        for (i = 0; i < 9; i++) {
            ncl_json_arr_push(batch, ncl_json_new_int(i));
        }
        ncl_json_arr_push(item->data, batch);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(item->data), 2);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_arr_get(item->data, 1)), 9);
        NCL_CHECK(ncl_message_sample_is_complete(msg));
        NCL_CHECK(ncl_message_is_valid(msg));
        NCL_CHECK_EQ_INT(ncl_sample_item_value_count(item), 19);
        NCL_CHECK_EQ_INT(ncl_message_sample_point_count(msg), 19);
        ncl_message_free(msg);
    }

    NCL_TEST_CASE("每槽点数抖动也照常上报（各列/各槽自便）");
    {
        ncl_node *config = config_from_json(
            "{\"id\":\"chJitter\",\"type\":\"SAMPLE_CHANNEL\","
            "\"sampleInterval\":40,\"uploadInterval\":80,"
            "\"ids\":[{\"id\":\"/JITTER@0\"}]}");
        int calls_before = tool.jitter_calls;
        size_t uploads_before = ncl_server_sample_upload_count(server);

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            NCL_CHECK_EQ_INT(ncl_server_add_sample(server, config), NCL_OK);
            ncl_node_free(config);
        }
        /* 工具第一槽给 10 点、之后 9 点：外层始终对齐，所以每个窗口都发 */
        for (i = 0; i < 200 && ncl_server_sample_upload_count(server) == uploads_before;
             i++) {
            ncl_sleep_millis(20);
        }
        NCL_CHECK(tool.jitter_calls > calls_before);
        NCL_CHECK(ncl_server_sample_upload_count(server) > uploads_before);
        NCL_CHECK_EQ_INT(ncl_server_remove_sample(server, "chJitter"), NCL_OK);
    }

    ncl_server_free(server);
    ncl_mqtt_client_destroy(mqtt);
    ncl_fake_server_stop(broker);
}

/**
 * Regression: ncl_server_free() used to release the tool bindings and the model
 * *before* stopping the sampling tasks. A task in the middle of its collect
 * cycle then queried the freed binding table — a use-after-free that surfaced
 * as a rare SIGSEGV inside ncl_server_lookup on a sampler thread (only when the
 * freed memory happened to be reused; the Go binding's allocator made it
 * reproducible, and it was found there first).
 *
 * The window is narrow in C (freed-but-untouched memory stays readable), so this
 * case is a smoke test for the teardown path: free with a channel running, no
 * explicit stop, twenty times. Its sharper counterpart lives in the Go suite
 * (TestServerFreeWithRunningSamples), whose allocator reuses the freed blocks
 * and turns the same shape into a hard crash.
 */
static void test_free_with_running_samples(void)
{
    /* A busy sampler: 1 ms slots over eight entries, so the task spends most of
     * its time inside ncl_server_lookup — exactly the window the old teardown
     * order blew up in. */
    static const char *kBusyModelJson =
        "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
        "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\","
        "\"configs\":[{\"id\":\"ch1\",\"type\":\"SAMPLE_CHANNEL\","
        "\"sampleInterval\":1,\"uploadInterval\":60000,"
        "\"ids\":[{\"id\":\"/STATUS\"},{\"id\":\"/STATUS\"},"
        "{\"id\":\"/STATUS\"},{\"id\":\"/STATUS\"},"
        "{\"id\":\"/STATUS\"},{\"id\":\"/STATUS\"},"
        "{\"id\":\"/STATUS\"},{\"id\":\"/STATUS\"}]}],"
        "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}]}]}";
    sample_tool tool;
    int round;

    NCL_TEST_CASE("freeing a server with a channel still running is safe");
    memset(&tool, 0, sizeof(tool));

    for (round = 0; round < 10; round++) {
        static ncl_tool_method methods[30];
        static ncl_tool_binding bindings[1];
        static const char *names[30] = {
            "m0",  "m1",  "m2",  "m3",  "m4",  "m5",  "m6",  "m7",  "m8",
            "m9",  "m10", "m11", "m12", "m13", "m14", "m15", "m16", "m17",
            "m18", "m19", "m20", "m21", "m22", "m23", "m24", "m25", "m26",
            "m27", "m28", "m29"};
        ncl_server_options options;
        ncl_server *server;
        size_t i;

        for (i = 0; i < 30; i++) {
            memset(&methods[i], 0, sizeof(methods[i]));
            methods[i].name = names[i];
            methods[i].fn = sample_get_status;
        }
        memset(bindings, 0, sizeof(bindings));
        bindings[0].path = "/STATUS";
        bindings[0].operation = NCL_OP_GET_VALUE;
        bindings[0].method = "m0";
        memset(&options, 0, sizeof(options));
        options.sn = TEST_SN;
        options.model_json = kBusyModelJson;
        server = ncl_server_create(&options);
        NCL_CHECK(server != NULL);
        if (server == NULL) {
            return;
        }
        if (ncl_server_register_tool(server, "sampleTool",
                                     &tool, methods, 30, bindings, 1) != NCL_OK ||
            ncl_server_init_samples(server) != NCL_OK) {
            NCL_CHECK(0);
            ncl_server_free(server);
            return;
        }
        ncl_sleep_millis(10);
        ncl_server_free(server);
    }
    NCL_CHECK(tool.status_calls > 0);
}

NCL_TEST_MAIN_BEGIN()
    test_server_end_to_end();
    test_sampling();
    test_sample_channel_shapes();
    test_sub_millisecond_samples();
    test_free_with_running_samples();
NCL_TEST_MAIN_END()
