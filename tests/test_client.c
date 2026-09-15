/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * End-to-end test of the NC-Link client layer against the fake server:
 * process wide client -> channel -> MQTT -> request/response correlation.
 *
 * The fake server answers each request with a real NC-Link response message,
 * so the whole stack is exercised: topic construction, message serialisation,
 * MQTT QoS 2 publish, inbound routing by serial number, response caching and
 * the typed convenience helpers.
 */
#include "ncl_test.h"

#include <stdlib.h>

#include "fake_nclink_server.h"
#include "nclink/ncl_client.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_topic.h"

#define TEST_SN "V203243111F"

/** 采样回调收到的内容，供断言使用。 */
typedef struct {
    int  samples;
    char channel[64];
    char path[128];
    int  interval;
    int  upload_interval;
    int  values;
    int  first_value;
} seen_sample;

static void on_sample(ncl_client *client, const char *topic,
                      const ncl_message *sample, void *user)
{
    seen_sample *seen = (seen_sample *)user;
    const char *channel = sample->as.sample.id;
    const ncl_sample_item *item;

    (void)client;
    (void)topic;
    seen->samples++;
    snprintf(seen->channel, sizeof(seen->channel), "%s",
             channel != NULL ? channel : "");
    seen->interval = (int)sample->as.sample.interval;
    seen->upload_interval = (int)sample->as.sample.upload_interval;
    snprintf(seen->path, sizeof(seen->path), "%s",
             ncl_strvec_len(&sample->as.sample.paths) > 0
                 ? ncl_strvec_at(&sample->as.sample.paths, 0)
                 : "");
    item = (const ncl_sample_item *)ncl_message_item_at(sample, 0);
    if (item != NULL) {
        long long first = -1;
        seen->values = (int)ncl_json_arr_len(item->data);
        ncl_json_as_int(ncl_json_arr_get(item->data, 0), &first);
        seen->first_value = (int)first;
    }
}

#ifndef NCL_TEST_DATA_DIR
#define NCL_TEST_DATA_DIR "."
#endif

static char g_model_json[1024];

/** Load a small valid device model for the probe response. */
static void load_model(void)
{
    /* A device with one data item, kept minimal but structurally valid. */
    snprintf(g_model_json, sizeof(g_model_json),
             "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
             "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[],"
             "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}],"
             "\"version\":\"2.0\"}],\"uniqueID\":\"test\"}");
}

/* ------------------------------------------------------- server responses -- */

/** Reply to query requests: values[0] = 42 (or an echo of the operation). */
static void answer_query(ncl_fake_server *server, const char *sn,
                         ncl_message *request, const char *operation)
{
    ncl_message *response = ncl_message_new(NCL_MSG_QUERY_RESPONSE);
    ncl_query_response_item *item;
    const ncl_query_request_item *req_item =
        (const ncl_query_request_item *)ncl_message_item_at(request, 0);
    char *topic = ncl_topic_query_response(sn, NULL);

    ncl_message_set_message_id(response, request->message_id);
    item = ncl_query_response_item_new(req_item != NULL ? req_item->id : "/x");
    item->code = ncl_strdup(NCL_KW_CODE_OK);
    if (operation != NULL && strcmp(operation, "get_length") == 0) {
        ncl_query_response_item_add_value(item, ncl_json_new_int(3));
    } else if (operation != NULL && strcmp(operation, "get_value") == 0 &&
               req_item != NULL && req_item->params != NULL &&
               ncl_params_has(req_item->params, "indexes")) {
        /* Range query: answer with a list of values. */
        ncl_json *values = ncl_json_new_array();
        ncl_json_arr_push(values, ncl_json_new_int(1));
        ncl_json_arr_push(values, ncl_json_new_int(2));
        ncl_query_response_item_add_value(item, values);
    } else {
        ncl_query_response_item_add_value(item, ncl_json_new_int(42));
    }
    ncl_message_add_query_response_item(response, item);
    {
        char *json = ncl_message_write_string(response);
        if (json != NULL) {
            ncl_fake_server_publish(server, topic, json, 2);
            free(json);
        }
    }
    ncl_message_free(response);
    free(topic);
}

static void answer_set(ncl_fake_server *server, const char *sn,
                       ncl_message *request)
{
    ncl_message *response = ncl_message_new(NCL_MSG_SET_RESPONSE);
    ncl_set_response_item *item;
    const ncl_set_request_item *req_item =
        (const ncl_set_request_item *)ncl_message_item_at(request, 0);
    char *topic = ncl_topic_set_response(sn, NULL);

    ncl_message_set_message_id(response, request->message_id);
    item = ncl_set_response_item_new(req_item != NULL ? req_item->id : "/x");
    item->code = ncl_strdup(NCL_KW_CODE_OK);
    ncl_message_add_set_response_item(response, item);
    {
        char *json = ncl_message_write_string(response);
        if (json != NULL) {
            ncl_fake_server_publish(server, topic, json, 2);
            free(json);
        }
    }
    ncl_message_free(response);
    free(topic);
}

static void answer_probe(ncl_fake_server *server, const char *sn)
{
    ncl_message *response = ncl_message_new(NCL_MSG_PROBE_QUERY_RESPONSE);
    char *topic = ncl_topic_probe_query_response(sn, NULL);

    ncl_message_set_code(response, NCL_KW_CODE_OK);
    ncl_message_set_model(response, ncl_root_node_parse(g_model_json));
    {
        char *json = ncl_message_write_string(response);
        if (json != NULL) {
            ncl_fake_server_publish(server, topic, json, 2);
            free(json);
        }
    }
    ncl_message_free(response);
    free(topic);
}

static void answer_method_call(ncl_fake_server *server, const char *sn,
                               ncl_message *request)
{
    ncl_message *response = ncl_message_new(NCL_MSG_METHOD_CALL_RESPONSE);
    char *topic = ncl_topic_method_call_response(sn, NULL);
    ncl_json *data = ncl_json_new_object();

    ncl_message_set_message_id(response, request->message_id);
    ncl_message_set_code(response, NCL_KW_CODE_OK);
    ncl_message_set_method(response, request->as.method_call_request.method);
    ncl_json_obj_set_bool(data, "added", true);
    ncl_message_set_data(response, data);
    {
        char *json = ncl_message_write_string(response);
        if (json != NULL) {
            ncl_fake_server_publish(server, topic, json, 2);
            free(json);
        }
    }
    ncl_message_free(response);
    free(topic);
}

/** Dispatch an inbound request to the server. */
static void on_request(ncl_fake_server *server, void *user, const char *topic,
                       const char *payload, size_t payload_len)
{
    ncl_message *request;
    const char *sn = TEST_SN;
    const char *operation = NULL;
    (void)user;

    request = ncl_message_parse(topic, payload, payload_len);
    if (request == NULL) {
        return;
    }
    if (request->type == NCL_MSG_QUERY_REQUEST) {
        const ncl_query_request_item *item =
            (const ncl_query_request_item *)ncl_message_item_at(request, 0);
        if (item != NULL) {
            operation = ncl_query_request_item_operation(item);
        }
        answer_query(server, sn, request, operation);
    } else if (request->type == NCL_MSG_SET_REQUEST) {
        answer_set(server, sn, request);
    } else if (request->type == NCL_MSG_PROBE_QUERY_REQUEST) {
        answer_probe(server, sn);
    } else if (request->type == NCL_MSG_METHOD_CALL_REQUEST) {
        answer_method_call(server, sn, request);
    }
    ncl_message_free(request);
}

/* ------------------------------------------------------------- predicates -- */

static bool pred_request_seen(ncl_fake_server *server)
{
    return ncl_fake_server_request_count(server) > 0;
}

static bool pred_subscribed(ncl_fake_server *server)
{
    return ncl_fake_server_subscribe_count(server) >= 1;
}

static bool pred_connected(ncl_fake_server *server)
{
    return ncl_fake_server_connection_count(server) >= 1;
}

/* ------------------------------------------------------------------ tests -- */

static void test_client_full_flow(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *server;
    ncl_client *client;
    char url[128];
    ncl_json *value = NULL;
    long long length = 0;
    seen_sample seen;

    load_model();
    memset(&seen, 0, sizeof(seen));
    memset(&fake_options, 0, sizeof(fake_options));
    fake_options.on_request = on_request;
    server = ncl_fake_server_start(&fake_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(server));

    NCL_TEST_CASE("the process wide client initialises and connects");
    NCL_CHECK_EQ_INT(ncl_client_holder_init(url, "admin", "123456"), NCL_OK);
    NCL_CHECK(ncl_client_holder_is_initialised());
    NCL_CHECK(ncl_fake_server_wait(server, pred_connected, 5000));

    NCL_TEST_CASE("holder hands out a client per serial number");
    client = ncl_client_holder_get(TEST_SN);
    NCL_CHECK(client != NULL);
    NCL_CHECK_EQ_STR(ncl_client_sn(client), TEST_SN);
    NCL_CHECK(ncl_client_holder_get(TEST_SN) == client); /* cached */
    NCL_CHECK(ncl_fake_server_wait(server, pred_subscribed, 5000));
    NCL_CHECK_EQ_INT(ncl_client_holder_client_count(), 1);

    NCL_TEST_CASE("getValue returns the queried value");
    NCL_CHECK_EQ_INT(ncl_client_get_value(client, "/STATUS", 5000, &value), NCL_OK);
    NCL_CHECK(value != NULL);
    {
        long long iv = 0;
        NCL_CHECK(ncl_json_as_int(value, &iv));
        NCL_CHECK_EQ_INT(iv, 42);
    }
    ncl_json_free(value);
    NCL_CHECK(ncl_fake_server_wait(server, pred_request_seen, 3000));
    NCL_CHECK_EQ_STR(ncl_fake_server_last_topic(server),
                     "Query/Request/" TEST_SN);
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "\"get_value\"") != NULL);
    NCL_CHECK_EQ_INT(ncl_fake_server_publish_count(server, 2), 1);

    NCL_TEST_CASE("getValue with an index range asks for indexes");
    value = NULL;
    NCL_CHECK_EQ_INT(ncl_client_get_value_range(client, "/LIST", 0, 2, 5000,
                                                &value),
                     NCL_OK);
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(value), 2);
    ncl_json_free(value);
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "0-2") != NULL);

    NCL_TEST_CASE("getLength returns the reported length");
    NCL_CHECK_EQ_INT(ncl_client_get_length(client, "/STATUS", 5000, &length),
                     NCL_OK);
    NCL_CHECK_EQ_INT(length, 3);
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "get_length") != NULL);

    NCL_TEST_CASE("setValue reports success");
    NCL_CHECK_EQ_INT(ncl_client_set_value(client, "/STATUS", ncl_json_new_int(7),
                                          5000),
                     NCL_OK);
    NCL_CHECK_EQ_STR(ncl_fake_server_last_topic(server), "Set/Request/" TEST_SN);
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "\"value\":7") != NULL);

    NCL_TEST_CASE("setValue with an index carries the index");
    NCL_CHECK_EQ_INT(ncl_client_set_value_index(client, "/LIST",
                                                ncl_json_new_string("x"), 2,
                                                5000),
                     NCL_OK);
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "\"index\":2") != NULL);

    NCL_TEST_CASE("probe installs the device model on the client");
    {
        ncl_message *probe = NULL;
        NCL_CHECK_EQ_INT(ncl_client_probe(client, 5000, &probe), NCL_OK);
        NCL_CHECK(probe != NULL);
        if (probe != NULL) {
            /* Detach so the client can take ownership of the model. */
            ncl_node *model = ncl_message_take_model(probe);
            NCL_CHECK(model != NULL);
            NCL_CHECK(ncl_node_is_valid(model));
            ncl_client_set_root_node(client, model);
            ncl_message_free(probe);
        }
    }

    NCL_TEST_CASE("the installed model resolves paths and identifiers");
    {
        char *id = ncl_client_get_id(client, "/STATUS");
        char *path;
        NCL_CHECK(id != NULL);
        if (id != NULL) {
            NCL_CHECK_EQ_STR(id, "030001");
            path = ncl_client_get_path(client, "030001");
            NCL_CHECK(path != NULL);
            if (path != NULL) {
                NCL_CHECK_EQ_STR(path, "/STATUS");
                free(path);
            }
            free(id);
        }
        NCL_CHECK(ncl_client_get_id(client, "/nope") == NULL);
    }

    NCL_TEST_CASE("methodCall addSample succeeds");
    {
        ncl_node *config = ncl_node_new(NCL_NODE_CONFIG);
        ncl_node_set_id(config, "ch1");
        ncl_node_set_type_name(config, "SAMPLE_CHANNEL");
        NCL_CHECK_EQ_INT(ncl_client_add_sample(client, config, 5000), NCL_OK);
        ncl_node_free(config);
    }
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "/nclinkServer/addSample")
              != NULL);

    NCL_TEST_CASE("removeSample succeeds");
    NCL_CHECK_EQ_INT(ncl_client_remove_sample(client, "ch1", 5000), NCL_OK);
    NCL_CHECK(strstr(ncl_fake_server_last_payload(server), "/nclinkServer/removeSample")
              != NULL);

    NCL_TEST_CASE("subscribing to the sample topic");
    NCL_CHECK_EQ_INT(ncl_client_subscribe_samples(client, 0), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_client_sample_topic(client), "Sample/" TEST_SN "/#");

    NCL_TEST_CASE("an inbound Sample reaches the handler");
    {
        ncl_message *sample = ncl_message_new(NCL_MSG_SAMPLE);
        ncl_sample_item *item = ncl_sample_item_new();
        char *payload;
        int i;

        ncl_message_set_message_id(sample, "s1");
        ncl_message_set_sample_id(sample, "ch1");
        ncl_message_set_sample_interval(sample, 1000);
        ncl_message_set_upload_interval(sample, 5000);
        ncl_message_add_sample_path(sample, "/STATUS");
        NCL_CHECK_EQ_INT(
            ncl_sample_item_add_value(item, ncl_json_new_int(42)), NCL_OK);
        NCL_CHECK_EQ_INT(
            ncl_sample_item_add_value(item, ncl_json_new_int(43)), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_message_add_sample_item(sample, item), NCL_OK);

        payload = ncl_message_write_string(sample);
        ncl_message_free(sample);
        NCL_CHECK(payload != NULL);
        ncl_client_set_sample_handler(client, on_sample, &seen);
        /* 模拟 broker 按订阅投递上报报文 */
        NCL_CHECK_EQ_INT(
            ncl_fake_server_publish(server, "Sample/" TEST_SN "/ch1", payload, 0),
            NCL_OK);
        free(payload);

        for (i = 0; i < 200 && seen.samples == 0; i++) {
            ncl_sleep_millis(10);
        }
        NCL_CHECK_EQ_INT(seen.samples, 1);
        NCL_CHECK_EQ_STR(seen.channel, "ch1");
        NCL_CHECK_EQ_INT(seen.interval, 1000);
        NCL_CHECK_EQ_INT(seen.upload_interval, 5000);
        NCL_CHECK_EQ_STR(seen.path, "/STATUS");
        NCL_CHECK_EQ_INT(seen.values, 2);
        NCL_CHECK_EQ_INT(seen.first_value, 42);
        NCL_CHECK_EQ_INT(ncl_client_sample_count(client), 1);
        ncl_client_set_sample_handler(client, NULL, NULL);
    }

    NCL_TEST_CASE("a sample without a handler is dropped, not leaked");
    {
        ncl_message *sample = ncl_message_new(NCL_MSG_SAMPLE);
        ncl_sample_item *item = ncl_sample_item_new();
        char *payload;

        ncl_message_set_message_id(sample, "s2");
        ncl_message_set_sample_id(sample, "ch2");
        ncl_message_add_sample_path(sample, "/STATUS");
        ncl_sample_item_add_value(item, ncl_json_new_int(1));
        ncl_message_add_sample_item(sample, item);
        payload = ncl_message_write_string(sample);
        ncl_message_free(sample);
        ncl_fake_server_publish(server, "Sample/" TEST_SN "/ch2", payload, 0);
        free(payload);
        ncl_sleep_millis(200);
        NCL_CHECK_EQ_INT(seen.samples, 1);
        NCL_CHECK_EQ_INT(ncl_client_sample_count(client), 1);
    }

    NCL_TEST_CASE("unsubscribing from the sample topic");
    NCL_CHECK_EQ_INT(ncl_client_unsubscribe_samples(client), NCL_OK);

    NCL_TEST_CASE("shutdown releases the holder");
    ncl_client_holder_shutdown();
    NCL_CHECK(!ncl_client_holder_is_initialised());
    ncl_fake_server_stop(server);
}

static void test_request_timeout(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *server;
    ncl_client *client;
    char url[128];
    ncl_json *value = NULL;

    /* The server never answers, so the request must time out. */
    memset(&fake_options, 0, sizeof(fake_options));
    fake_options.on_request = NULL;
    server = ncl_fake_server_start(&fake_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(server));

    NCL_TEST_CASE("a request without a response reports a timeout");
    NCL_CHECK_EQ_INT(ncl_client_holder_init(url, NULL, NULL), NCL_OK);
    client = ncl_client_holder_get("V1");
    NCL_CHECK(client != NULL);
    if (client != NULL) {
        NCL_CHECK_EQ_INT(ncl_client_get_value(client, "/STATUS", 700, &value),
                         NCL_ERR_TIMEOUT);
        NCL_CHECK(value == NULL);
    }
    ncl_client_holder_shutdown();
    ncl_fake_server_stop(server);
}

static void test_holder_before_init(void)
{
    NCL_TEST_CASE("the holder refuses to hand out clients before init");
    ncl_client_holder_shutdown();
    NCL_CHECK(ncl_client_holder_get("V1") == NULL);
    NCL_CHECK(!ncl_client_holder_is_initialised());
    NCL_CHECK_EQ_INT(ncl_client_holder_init(NULL, NULL, NULL), NCL_ERR_INVALID_ARG);
}

/**
 * ncl_client_holder_restart() re-reads conf/mqtt.cfg, which is how a caller
 * applies a broker change made through the configuration REST API.
 */
static void test_holder_restart(void)
{
    ncl_fake_server_options fake_options;
    ncl_fake_server *server;
    char url[256];
    char cfg[512];
    char cfg_path[NCL_PATH_MAX_BUF];
    char root[] = "ncl_client_test_root";

    memset(&fake_options, 0, sizeof(fake_options));
    server = ncl_fake_server_start(&fake_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", ncl_fake_server_port(server));
    snprintf(cfg, sizeof(cfg), "url=%s\nusername=admin\npassword=123456\n", url);

    NCL_TEST_CASE("restart picks up conf/mqtt.cfg");
    ncl_env_set_root(root);
    NCL_CHECK_EQ_INT(ncl_mkdir_p(ncl_env_conf_path()), NCL_OK);
    snprintf(cfg_path, sizeof(cfg_path), "%s", ncl_env_mqtt_cfg_file());
    NCL_CHECK_EQ_INT(ncl_file_write_all(cfg_path, cfg, strlen(cfg)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_client_holder_restart(), NCL_OK);
    NCL_CHECK(ncl_client_holder_is_initialised());
    NCL_CHECK(ncl_client_holder_get("V9") != NULL);
    NCL_CHECK(ncl_fake_server_connection_count(server) >= 1);

    ncl_client_holder_shutdown();
    NCL_CHECK(!ncl_client_holder_is_initialised());
    ncl_fake_server_stop(server);
    ncl_env_set_root(NULL);
    NCL_CHECK_EQ_INT(ncl_path_remove(root), NCL_OK);
}

NCL_TEST_MAIN_BEGIN()
    test_client_full_flow();
    test_request_timeout();
    test_holder_before_init();
    test_holder_restart();
NCL_TEST_MAIN_END()
