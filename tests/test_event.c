/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Event push (Event/<sn>) and the parameter
 * validation of method calls (the "check" flag).
 *
 * The server is exercised through its publish hook, so no broker is needed:
 * every outbound message is captured and delivered to the client by hand, which
 * also lets the test assert the exact wire bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncl_test.h"
#include "nclink/ncl_client.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_schema.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_topic.h"

#define TEST_SN "V203243111F"

#define PUBLISH_SLOTS 8

typedef struct {
    ncl_server *server;
    ncl_client *client; /**< optional, receives everything published */
    int         publishes;
    char        topic[PUBLISH_SLOTS][256];
    char        payload[PUBLISH_SLOTS][2048];
    int         head;
} capture;

static ncl_err capture_publish(void *user, const char *topic,
                               const char *payload, size_t len)
{
    capture *cap = (capture *)user;
    int slot = cap->head % PUBLISH_SLOTS;

    snprintf(cap->topic[slot], sizeof(cap->topic[slot]), "%s", topic);
    snprintf(cap->payload[slot], sizeof(cap->payload[slot]), "%.*s",
             (int)(len < sizeof(cap->payload[slot]) - 1
                       ? len
                       : sizeof(cap->payload[slot]) - 1),
             payload);
    cap->head++;
    cap->publishes++;

    /* Everything arrives through the MQTT callback. */
    if (cap->client != NULL) {
        ncl_message *message = ncl_message_parse(topic, payload, len);
        if (message != NULL) {
            ncl_client_on_message(cap->client, topic, message);
        }
    }
    return NCL_OK;
}

/* ------------------------------------------------------------------ tools -- */

typedef struct {
    int calls;
} counting_tool;

/** A channel stub that records subscribe/unsubscribe calls. */
typedef struct {
    ncl_message_channel channel; /**< must be first */
    int                 subscribes;
    int                 unsubscribes;
    char                last_topic[256];
} stub_channel;

static ncl_err stub_publish(ncl_message_channel *self, const char *topic,
                            const ncl_message *message, int qos,
                            const ncl_mqtt_properties *properties)
{
    (void)self;
    (void)topic;
    (void)message;
    (void)qos;
    (void)properties;
    return NCL_OK;
}

static ncl_err stub_subscribe(ncl_message_channel *self, const char *topic,
                              int qos)
{
    stub_channel *stub = (stub_channel *)self;
    (void)qos;
    stub->subscribes++;
    snprintf(stub->last_topic, sizeof(stub->last_topic), "%s", topic);
    return NCL_OK;
}

static ncl_err stub_unsubscribe(ncl_message_channel *self, const char *topic)
{
    stub_channel *stub = (stub_channel *)self;
    stub->unsubscribes++;
    snprintf(stub->last_topic, sizeof(stub->last_topic), "%s", topic);
    return NCL_OK;
}

/** What the client event handler saw. */
typedef struct {
    int  count;
    char id[64];
    char key[64];
    int  value;
} seen_event;

static void on_event(ncl_client *client, const char *topic,
                     const ncl_message *event, void *user)
{
    seen_event *seen = (seen_event *)user;
    const char *id;
    const char *key;

    (void)client;
    (void)topic;
    seen->count++;
    id = event->as.event.id;
    key = ncl_json_obj_get_string(event->as.event.event, "key");
    snprintf(seen->id, sizeof(seen->id), "%s", id != NULL ? id : "");
    snprintf(seen->key, sizeof(seen->key), "%s", key != NULL ? key : "");
    seen->value =
        (int)ncl_json_obj_get_int(event->as.event.event, "value", -1);
}

static ncl_err tool_status(void *instance, const ncl_json *params,
                           ncl_json **result, char **reason)
{
    counting_tool *tool = (counting_tool *)instance;
    (void)params;
    (void)reason;
    tool->calls++;
    *result = ncl_json_new_int(1);
    return NCL_OK;
}

static ncl_err tool_set_speed(void *instance, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    counting_tool *tool = (counting_tool *)instance;
    (void)params;
    (void)reason;
    tool->calls++;
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

static ncl_err tool_bare(void *instance, const ncl_json *params,
                         ncl_json **result, char **reason)
{
    counting_tool *tool = (counting_tool *)instance;
    (void)params;
    (void)reason;
    tool->calls++;
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

#define SPEED_SCHEMA                                                           \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"speed\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"          \
    "\"mode\":{\"enum\":[\"fast\",\"slow\"]}},"                                \
    "\"required\":[\"speed\"]}"

static const ncl_tool_method k_methods[] = {
    {"getValue", tool_status, NULL},
    {"setSpeed", tool_set_speed, SPEED_SCHEMA},
    {"bare", tool_bare, NULL}};

static const ncl_tool_binding k_bindings[] = {
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", "plant"}};

/* --------------------------------------------------------------- helpers -- */

/** Send a MethodCall through the server and hand back the response. */
static ncl_message *call(ncl_server *server, const char *method,
                         const char *params_json, bool check)
{
    ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    ncl_message *response;

    ncl_message_set_message_id(request, "call-1");
    ncl_message_set_method(request, method);
    if (params_json != NULL) {
        ncl_message_set_params(request, ncl_json_parse_cstr(params_json, NULL));
    }
    ncl_message_set_check(request, check);
    response = ncl_server_invoke_method_call(server, request);
    ncl_message_free(request);
    return response;
}

static const char *call_code(ncl_message *response)
{
    if (response == NULL || response->type != NCL_MSG_METHOD_CALL_RESPONSE) {
        return "(none)";
    }
    return response->as.method_call_response.code != NULL
               ? response->as.method_call_response.code
               : "(null)";
}

static const char *call_reason(ncl_message *response)
{
    return response != NULL && response->type == NCL_MSG_METHOD_CALL_RESPONSE &&
                   response->as.method_call_response.reason != NULL
               ? response->as.method_call_response.reason
               : "";
}

/* ==================================================================== main */

NCL_TEST_MAIN_BEGIN()

    capture cap;
    ncl_server_options options;
    ncl_server *server;
    counting_tool tool;
    ncl_client *client;
    stub_channel stub;
    seen_event seen;

    memset(&cap, 0, sizeof(cap));
    memset(&tool, 0, sizeof(tool));
    memset(&stub, 0, sizeof(stub));
    memset(&seen, 0, sizeof(seen));
    stub.channel.publish = stub_publish;
    stub.channel.subscribe = stub_subscribe;
    stub.channel.unsubscribe = stub_unsubscribe;

    NCL_TEST_CASE("the event topic");
    {
        char *topic = ncl_topic_event(TEST_SN, NULL);
        NCL_CHECK_EQ_STR(topic, "Event/" TEST_SN);
        free(topic);
        NCL_CHECK_EQ_STR(NCL_TOPIC_EVENT_PREFIX, "Event/");
    }

    NCL_TEST_CASE("the Event message serialises with the expected fields");
    {
        ncl_message *message = ncl_message_new(NCL_MSG_EVENT);
        ncl_json *event = ncl_json_new_object();
        char *text;

        ncl_json_obj_set_string(event, "key", "STATUS");
        ncl_json_obj_set_int(event, "value", 7);
        ncl_message_set_message_id(message, "evt-1");
        ncl_message_set_sample_id(message, "030001");
        ncl_message_set_event(message, event);
        NCL_CHECK_EQ_INT(ncl_message_finalise(message), NCL_OK);
        NCL_CHECK(message->as.event.time != NULL); /* build() filled it in */

        text = ncl_message_write_string(message);
        NCL_CHECK(strstr(text, "\"@id\":\"evt-1\"") != NULL);
        NCL_CHECK(strstr(text, "\"id\":\"030001\"") != NULL);
        NCL_CHECK(strstr(text, "\"time\":\"") != NULL);
        NCL_CHECK(strstr(text, "\"event\":{\"key\":\"STATUS\",\"value\":7}") !=
                  NULL);
        free(text);

        /* Round trip. */
        text = ncl_message_write_string(message);
        {
            ncl_message *parsed =
                ncl_message_parse("Event/" TEST_SN, text, strlen(text));
            NCL_CHECK(parsed != NULL);
            if (parsed != NULL) {
                NCL_CHECK_EQ_INT(parsed->type, NCL_MSG_EVENT);
                NCL_CHECK_EQ_STR(parsed->as.event.id, "030001");
                NCL_CHECK_EQ_INT(
                    ncl_json_obj_get_int(parsed->as.event.event, "value", 0), 7);
                NCL_CHECK(ncl_message_is_valid(parsed));
                ncl_message_free(parsed);
            }
        }
        free(text);
        ncl_message_free(message);
    }

    NCL_TEST_CASE("an Event without an id or event body is invalid");
    {
        ncl_message *message = ncl_message_new(NCL_MSG_EVENT);
        ncl_json *event = ncl_json_new_object();
        ncl_message_set_message_id(message, "evt-2");
        ncl_message_set_event(message, event); /* id still missing */
        NCL_CHECK(!ncl_message_is_valid(message));
        NCL_CHECK_EQ_INT(ncl_message_finalise(message), NCL_ERR_INVALID_MESSAGE);
        ncl_message_free(message);
    }

    /* ---------------------------------------------------------- server ---- */
    memset(&options, 0, sizeof(options));
    options.sn = TEST_SN;
    options.publish = capture_publish;
    options.publish_user = &cap;
    server = ncl_server_create(&options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        printf("cannot create the server\n");
        return 1;
    }
    cap.server = server;
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "plant", &tool, k_methods,
                                              sizeof(k_methods) / sizeof(k_methods[0]),
                                              k_bindings,
                                              sizeof(k_bindings) / sizeof(k_bindings[0])),
                     NCL_OK);

    NCL_TEST_CASE("ncl_server_push_event publishes on Event/<sn>");
    {
        ncl_json *event = ncl_json_new_object();
        int before = cap.publishes;

        ncl_json_obj_set_string(event, "key", "PART_COUNT");
        ncl_json_obj_set_int(event, "value", 12);
        ncl_json_obj_set_int(event, "oldValue", 11);
        NCL_CHECK_EQ_INT(ncl_server_push_event(server, "030002", event), NCL_OK);
        NCL_CHECK_EQ_INT(cap.publishes, before + 1);
        NCL_CHECK_EQ_STR(cap.topic[(cap.head - 1) % PUBLISH_SLOTS],
                         "Event/" TEST_SN);
        NCL_CHECK(strstr(cap.payload[(cap.head - 1) % PUBLISH_SLOTS],
                         "\"id\":\"030002\"") != NULL);
        NCL_CHECK(strstr(cap.payload[(cap.head - 1) % PUBLISH_SLOTS],
                         "\"oldValue\":11") != NULL);
        NCL_CHECK_EQ_INT(ncl_server_event_count(server), 1);
        ncl_json_free(event);
    }

    NCL_TEST_CASE("push_event validates its arguments");
    {
        ncl_json *event = ncl_json_new_object();
        NCL_CHECK_EQ_INT(ncl_server_push_event(server, NULL, event),
                         NCL_ERR_INVALID_ARG);
        NCL_CHECK_EQ_INT(ncl_server_push_event(server, "x", NULL),
                         NCL_ERR_INVALID_ARG);
        NCL_CHECK_EQ_INT(ncl_server_push_event_ex(server, "id", event, 1234,
                                                  "fixed-id"),
                         NCL_OK);
        NCL_CHECK(strstr(cap.payload[(cap.head - 1) % PUBLISH_SLOTS],
                         "\"@id\":\"fixed-id\"") != NULL);
        NCL_CHECK(strstr(cap.payload[(cap.head - 1) % PUBLISH_SLOTS],
                         "\"time\":\"1234\"") != NULL);
        ncl_json_free(event);
    }

    /* ------------------------------------------------------- method check -- */
    NCL_TEST_CASE("check=true validates instead of executing");
    {
        ncl_message *response =
            call(server, "/plant/setSpeed", "{\"speed\":10}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_OK);
        NCL_CHECK_EQ_INT(tool.calls, 0);
        ncl_message_free(response);
    }

    NCL_TEST_CASE("check=true reports every violation");
    {
        ncl_message *response = call(
            server, "/plant/setSpeed",
            "{\"speed\":500,\"mode\":\"warp\"}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_NG);
        NCL_CHECK(strstr(call_reason(response), "#/speed") != NULL);
        NCL_CHECK(strstr(call_reason(response), "#/mode") != NULL);
        NCL_CHECK(strstr(call_reason(response), "[") != NULL);
        NCL_CHECK_EQ_INT(tool.calls, 0);
        ncl_message_free(response);
    }

    NCL_TEST_CASE("check=true catches missing required parameters");
    {
        ncl_message *response = call(server, "/plant/setSpeed", "{}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_NG);
        NCL_CHECK(strstr(call_reason(response), "required key [speed]") != NULL);
        NCL_CHECK_EQ_INT(tool.calls, 0);
        ncl_message_free(response);
    }

    NCL_TEST_CASE("a method without a schema accepts only empty parameters");
    {
        ncl_message *ok = call(server, "/plant/bare", NULL, true);
        NCL_CHECK_EQ_STR(call_code(ok), NCL_KW_CODE_OK);
        ncl_message_free(ok);

        ok = call(server, "/plant/bare", "{}", true);
        NCL_CHECK_EQ_STR(call_code(ok), NCL_KW_CODE_OK);
        ncl_message_free(ok);

        ok = call(server, "/plant/bare", "{\"a\":1}", true);
        NCL_CHECK_EQ_STR(call_code(ok), NCL_KW_CODE_NG);
        NCL_CHECK_EQ_STR(call_reason(ok), "参数数量不匹配");
        ncl_message_free(ok);
        NCL_CHECK_EQ_INT(tool.calls, 0);
    }

    NCL_TEST_CASE("check=true on an unknown method");
    {
        ncl_message *response = call(server, "/plant/nope", "{}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_NG);
        NCL_CHECK_EQ_STR(call_reason(response), "没有找到方法");
        ncl_message_free(response);
    }

    NCL_TEST_CASE("check is echoed back, and false still executes");
    {
        ncl_message *response =
            call(server, "/plant/setSpeed", "{\"speed\":10}", false);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_OK);
        NCL_CHECK_EQ_INT(tool.calls, 1);
        ncl_message_free(response);

        response = call(server, "/plant/setSpeed", "{\"speed\":10}", true);
        NCL_CHECK(response->as.method_call_response.has_check);
        NCL_CHECK(response->as.method_call_response.check);
        ncl_message_free(response);
    }

    NCL_TEST_CASE("a query binding still works with schemas present");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item = ncl_query_request_item_new("/STATUS");
        ncl_message *response;
        int before = tool.calls;

        ncl_message_set_message_id(request, "q1");
        ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(server, request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *result =
                (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(result != NULL);
            NCL_CHECK_EQ_STR(result->code, NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
        NCL_CHECK_EQ_INT(tool.calls, before + 1);
    }

    NCL_TEST_CASE("the file tool declares parameter schemas");
    {
        ncl_message *response;
        char root[] = "ncl_event_test_root";

        /* Keep the configuration ncl_server_register_file_tool reads (and the
         * serial number it creates) inside a scratch directory. */
        ncl_env_set_root(root);
        NCL_CHECK_EQ_INT(ncl_server_register_file_tool(server), NCL_OK);

        response = call(server, "/file/write", "{}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_NG);
        NCL_CHECK(strstr(call_reason(response), "required key [key]") != NULL);
        ncl_message_free(response);

        response = call(server, "/file/write", "{\"key\":\"/a.txt\"}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_OK);
        ncl_message_free(response);

        /* read accepts either a key list or a local name (anyOf). */
        response = call(server, "/file/read", "{}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_NG);
        NCL_CHECK(strstr(call_reason(response), "anyOf") != NULL);
        ncl_message_free(response);

        response = call(server, "/file/read", "{\"keys\":[\"/a.txt\"]}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_OK);
        ncl_message_free(response);

        response = call(server, "/file/read", "{\"localname\":\"a.txt\"}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_OK);
        ncl_message_free(response);

        response = call(server, "/file/ll", "{\"keys\":\"nope\"}", true);
        NCL_CHECK_EQ_STR(call_code(response), NCL_KW_CODE_NG);
        NCL_CHECK(strstr(call_reason(response), "#/keys") != NULL);
        ncl_message_free(response);

        ncl_env_set_root(NULL);
        NCL_CHECK_EQ_INT(ncl_path_remove(root), NCL_OK);
    }

    /* --------------------------------------------------------- client ----- */
    NCL_TEST_CASE("the client routes events to its handler");
    {
        client = ncl_client_create(TEST_SN, &stub.channel);
        NCL_CHECK(client != NULL);
        cap.client = client;

        NCL_CHECK_EQ_INT(ncl_client_subscribe_events(client, 2), NCL_OK);
        NCL_CHECK_EQ_INT(stub.subscribes, 1);
        NCL_CHECK_EQ_STR(stub.last_topic, "Event/" TEST_SN);
        ncl_client_set_event_handler(client, on_event, &seen);
        {
            ncl_json *event = ncl_json_new_object();
            ncl_json_obj_set_string(event, "key", "TEMP");
            ncl_json_obj_set_int(event, "value", 42);
            NCL_CHECK_EQ_INT(ncl_server_push_event(server, "T1", event), NCL_OK);
            ncl_json_free(event);
        }
        NCL_CHECK_EQ_INT(seen.count, 1);
        NCL_CHECK_EQ_STR(seen.id, "T1");
        NCL_CHECK_EQ_STR(seen.key, "TEMP");
        NCL_CHECK_EQ_INT(seen.value, 42);
        NCL_CHECK_EQ_INT(ncl_client_event_count(client), 1);

        NCL_TEST_CASE("an event with no handler is dropped, not leaked");
        ncl_client_set_event_handler(client, NULL, NULL);
        {
            ncl_json *event = ncl_json_new_object();
            ncl_json_obj_set_string(event, "key", "X");
            NCL_CHECK_EQ_INT(ncl_server_push_event(server, "T2", event), NCL_OK);
            ncl_json_free(event);
        }
        NCL_CHECK_EQ_INT(seen.count, 1);

        NCL_TEST_CASE("unsubscribing from the event topic");
        NCL_CHECK_EQ_INT(ncl_client_unsubscribe_events(client), NCL_OK);
        NCL_CHECK_EQ_INT(stub.unsubscribes, 1);
        NCL_CHECK_EQ_STR(stub.last_topic, "Event/" TEST_SN);

        cap.client = NULL;
        ncl_client_free(client);
        client = NULL;
    }

    ncl_server_free(server);

NCL_TEST_MAIN_END()
