/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Wire format tests: every message kind is built through the C API, serialised
 * and compared with the expected JSON.
 */
#include "ncl_test.h"

#include "nclink/ncl_message.h"
#include "nclink/ncl_topic.h"

#ifndef NCL_TEST_DATA_DIR
#define NCL_TEST_DATA_DIR "."
#endif

static void check_wire(ncl_message *msg, const char *expected, const char *what)
{
    char *text = ncl_message_write_string(msg);
    char *finalised;

    NCL_TEST_CASE(what);
    NCL_CHECK(msg != NULL);
    NCL_CHECK(text != NULL);
    NCL_CHECK_EQ_STR(text, expected);

    /* The serialised form must parse back into an equivalent message. */
    if (text != NULL) {
        char topic[64];
        ncl_message *reparsed;
        snprintf(topic, sizeof(topic), "x/%s", "");
        finalised = ncl_message_write_string(msg);
        reparsed = ncl_message_from_json(msg->type,
                                        ncl_json_parse_cstr(text, NULL));
        NCL_CHECK(reparsed != NULL);
        if (reparsed != NULL) {
            char *again = ncl_message_write_string(reparsed);
            NCL_CHECK_EQ_STR(again, text);
            free(again);
            ncl_message_free(reparsed);
        }
        free(finalised);
    }
    free(text);
    ncl_message_free(msg);
}

static void test_simple_messages(void)
{
    ncl_message *m;

    m = ncl_message_new(NCL_MSG_PING);
    ncl_message_set_message_id(m, "m1");
    check_wire(m, "{\"@id\":\"m1\"}", "Ping carries only @id");

    m = ncl_message_new(NCL_MSG_PONG);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_open_api_schema(m, "{}");
    check_wire(m, "{\"@id\":\"m1\",\"OpenApiSchema\":\"{}\"}", "Pong / OpenApiSchema");

    m = ncl_message_new(NCL_MSG_PROBE_VERSION);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_version(m, "2.0");
    check_wire(m, "{\"@id\":\"m1\",\"version\":\"2.0\"}", "ProbeVersion");

    m = ncl_message_new(NCL_MSG_REGISTER_REQUEST);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_device_id(m, "V203243111F");
    check_wire(m, "{\"@id\":\"m1\",\"deviceid\":\"V203243111F\"}", "RegisterRequest");

    m = ncl_message_new(NCL_MSG_REGISTER_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_code(m, "OK");
    check_wire(m, "{\"@id\":\"m1\",\"code\":\"OK\"}", "RegisterResponse");

    m = ncl_message_new(NCL_MSG_PROBE_QUERY_REQUEST);
    ncl_message_set_message_id(m, "m1");
    check_wire(m, "{\"@id\":\"m1\"}", "ProbeQueryRequest");

    m = ncl_message_new(NCL_MSG_PROBE_SET_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_code(m, "OK");
    check_wire(m, "{\"@id\":\"m1\",\"code\":\"OK\"}", "ProbeSetResponse");
}

static void test_query(void)
{
    ncl_message *m;
    ncl_query_request_item *req;
    ncl_query_response_item *res;

    m = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_message_set_message_id(m, "m1");
    req = ncl_query_request_item_new("/STATUS");
    ncl_params_set_string(&req->params, "operation", "get_value");
    ncl_message_add_query_request_item(m, req);
    check_wire(m, "{\"@id\":\"m1\",\"ids\":[{\"id\":\"/STATUS\","
                 "\"params\":{\"operation\":\"get_value\"}}]}",
               "QueryRequest with params");

    m = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_message_set_message_id(m, "m1");
    req = ncl_query_request_item_new("/STATUS");
    ncl_params_set_string(&req->params, "operation", "get_value");
    ncl_params_append_string(&req->params, "indexes", "1");
    ncl_params_append_string(&req->params, "indexes", "3-5");
    ncl_message_add_query_request_item(m, req);
    check_wire(m, "{\"@id\":\"m1\",\"ids\":[{\"id\":\"/STATUS\",\"params\":"
                 "{\"operation\":\"get_value\",\"indexes\":[\"1\",\"3-5\"]}}]}",
               "QueryRequest with index range");

    m = ncl_message_new(NCL_MSG_QUERY_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    res = ncl_query_response_item_new("/STATUS");
    res->code = ncl_strdup("OK");
    ncl_query_response_item_add_value(res, ncl_json_new_int(42));
    ncl_message_add_query_response_item(m, res);
    check_wire(m, "{\"@id\":\"m1\",\"values\":[{\"id\":\"/STATUS\",\"code\":\"OK\","
                 "\"values\":[42]}]}",
               "QueryResponse with value");

    m = ncl_message_new(NCL_MSG_QUERY_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    res = ncl_query_response_item_new("/STATUS");
    res->code = ncl_strdup("NG");
    res->reason = ncl_strdup("NOT_FOUND");
    ncl_message_add_query_response_item(m, res);
    check_wire(m, "{\"@id\":\"m1\",\"values\":[{\"id\":\"/STATUS\",\"code\":\"NG\","
                 "\"reason\":\"NOT_FOUND\"}]}",
               "QueryResponse with reason");
}

static void test_set(void)
{
    ncl_message *m;
    ncl_set_request_item *req;
    ncl_set_response_item *res;

    m = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_message_set_message_id(m, "m1");
    req = ncl_set_request_item_new("/STATUS");
    ncl_params_set_string(&req->params, "operation", "set_value");
    ncl_params_set(&req->params, "value", ncl_json_new_int(7));
    ncl_message_add_set_request_item(m, req);
    check_wire(m, "{\"@id\":\"m1\",\"values\":[{\"id\":\"/STATUS\",\"params\":"
                 "{\"operation\":\"set_value\",\"value\":7}}]}",
               "SetRequest");

    m = ncl_message_new(NCL_MSG_SET_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    res = ncl_set_response_item_new("/STATUS");
    res->code = ncl_strdup("OK");
    ncl_message_add_set_response_item(m, res);
    check_wire(m, "{\"@id\":\"m1\",\"results\":[{\"id\":\"/STATUS\",\"code\":\"OK\"}]}",
               "SetResponse");
}

static void test_sample_and_event(void)
{
    ncl_message *m;
    ncl_sample_item *item;

    m = ncl_message_new(NCL_MSG_SAMPLE);
    ncl_message_set_message_id(m, "m1");
    ncl_message_add_sample_path(m, "/STATUS");
    ncl_message_set_sample_id(m, "s1");
    ncl_message_set_begin_time(m, "1700000000000");
    item = ncl_sample_item_new();
    ncl_sample_item_add_value(item, ncl_json_new_int(1));
    ncl_sample_item_add_value(item, ncl_json_new_int(2));
    ncl_message_add_sample_item(m, item);
    ncl_message_set_sample_interval(m, 1000);
    ncl_message_set_upload_interval(m, 2000);
    check_wire(m, "{\"@id\":\"m1\",\"paths\":[\"/STATUS\"],\"id\":\"s1\","
                 "\"beginTime\":\"1700000000000\",\"data\":[{\"data\":[1,2]}],"
                 "\"interval\":1000,\"uploadInterval\":2000}",
               "Sample message");

    m = ncl_message_new(NCL_MSG_EVENT);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_sample_id(m, "e1");
    {
        ncl_json *event = ncl_json_new_object();
        ncl_json_obj_set_int(event, "value", 1);
        ncl_message_set_event(m, event);
    }
    /* Event.time is a number rendered as a string on the wire. */
    {
        ncl_message *ev = m;
        char *text;
        ev->as.event.time = ncl_strdup("1700000000000");
        text = ncl_message_write_string(ev);
        NCL_TEST_CASE("Event message");
        NCL_CHECK_EQ_STR(text, "{\"@id\":\"m1\",\"id\":\"e1\","
                               "\"time\":\"1700000000000\",\"event\":{\"value\":1}}");
        free(text);
        ncl_message_free(ev);
    }
}

static void test_method_call(void)
{
    ncl_message *m;
    ncl_json *params;
    ncl_json *keys;

    m = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_method(m, "/file/read");
    params = ncl_json_new_object();
    keys = ncl_json_new_array();
    ncl_json_arr_push(keys, ncl_json_new_string("/bin/yyy/1111.txt"));
    ncl_json_obj_set(params, "keys", keys);
    ncl_message_set_params(m, params);
    check_wire(m, "{\"@id\":\"m1\",\"method\":\"/file/read\",\"params\":"
                 "{\"keys\":[\"/bin/yyy/1111.txt\"]},\"check\":false}",
               "MethodCallRequest (check defaults to false and is emitted)");

    m = ncl_message_new(NCL_MSG_METHOD_CALL_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_code(m, "OK");
    ncl_message_set_method(m, "/file/read");
    {
        ncl_json *data = ncl_json_new_object();
        ncl_json_obj_set_bool(data, "ok", true);
        ncl_message_set_data(m, data);
    }
    check_wire(m, "{\"@id\":\"m1\",\"code\":\"OK\",\"method\":\"/file/read\","
                 "\"data\":{\"ok\":true}}",
               "MethodCallResponse");
}

static void test_probe_with_model(void)
{
    char path[1024];
    char *source = NULL;
    ncl_node *root;
    ncl_message *m;
    char *text;
    ncl_strbuf expected;

    snprintf(path, sizeof(path), "%s/model_nclink.json", NCL_TEST_DATA_DIR);
    if (ncl_file_read_all(path, &source, NULL) != NCL_OK) {
        printf("    cannot read %s, skipping\n", path);
        return;
    }
    root = ncl_root_node_parse(source);
    free(source);
    NCL_CHECK(root != NULL);
    if (root == NULL) {
        return;
    }

    NCL_TEST_CASE("ProbeQueryResponse embeds the model under \"probe\"");
    m = ncl_message_new(NCL_MSG_PROBE_QUERY_RESPONSE);
    ncl_message_set_message_id(m, "m1");
    ncl_message_set_code(m, "OK");
    ncl_message_set_model(m, root);
    text = ncl_message_write_string(m);
    NCL_CHECK(text != NULL);
    if (text != NULL) {
        ncl_strbuf_init(&expected);
        ncl_strbuf_puts(&expected, "{\"@id\":\"m1\",\"code\":\"OK\",\"probe\":");
        {
            char *model_text = ncl_node_write_string(root);
            ncl_strbuf_puts(&expected, model_text);
            free(model_text);
        }
        ncl_strbuf_puts(&expected, "}");
        NCL_CHECK_EQ_STR(text, ncl_strbuf_cstr(&expected));
        ncl_strbuf_free(&expected);

        NCL_TEST_CASE("the embedded model round trips through parse");
        {
            ncl_json *json = ncl_json_parse_cstr(text, NULL);
            ncl_message *back = ncl_message_from_json(NCL_MSG_PROBE_QUERY_RESPONSE, json);
            NCL_CHECK(back != NULL);
            if (back != NULL) {
                NCL_CHECK(ncl_node_is_valid(back->as.probe_query_response.model));
                ncl_message_free(back);
            }
            ncl_json_free(json);
        }
        free(text);
    }
    ncl_message_free(m);
}

static void test_validation_and_finalise(void)
{
    ncl_message *m = ncl_message_new(NCL_MSG_QUERY_REQUEST);

    NCL_TEST_CASE("a QueryRequest without items is invalid");
    NCL_CHECK(!ncl_message_is_valid(m));
    NCL_CHECK_EQ_INT(ncl_message_finalise(m), NCL_ERR_INVALID_MESSAGE);

    NCL_TEST_CASE("adding a valid item makes it valid and finalise assigns a UUID");
    ncl_message_add_query_request_item(m, ncl_query_request_item_new("/STATUS"));
    NCL_CHECK(ncl_message_is_valid(m));
    NCL_CHECK_EQ_INT(ncl_message_finalise(m), NCL_OK);
    NCL_CHECK(m->message_id != NULL);
    NCL_CHECK_EQ_INT(strlen(m->message_id), 36);
    ncl_message_free(m);

    NCL_TEST_CASE("insertion order of params is preserved");
    m = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_message_set_message_id(m, "m1");
    {
        ncl_set_request_item *item = ncl_set_request_item_new("/A");
        ncl_params_set_int(&item->params, "offset", 1);
        ncl_params_set_string(&item->params, "operation", "set_value");
        ncl_params_set(&item->params, "value", ncl_json_new_string("x"));
        ncl_params_set_int(&item->params, "length", 3);
        ncl_message_add_set_request_item(m, item);
    }
    {
        char *text = ncl_message_write_string(m);
        NCL_CHECK_EQ_STR(text, "{\"@id\":\"m1\",\"values\":[{\"id\":\"/A\",\"params\":"
                               "{\"offset\":1,\"operation\":\"set_value\","
                               "\"value\":\"x\",\"length\":3}}]}");
        free(text);
    }
    ncl_message_free(m);
}

static void test_parse_by_topic(void)
{
    const char *payload =
        "{\"@id\":\"m1\",\"values\":[{\"id\":\"/STATUS\",\"code\":\"OK\","
        "\"values\":[{\"number\":3}]}]}";
    ncl_message *msg;
    ncl_query_response_item *item;

    NCL_TEST_CASE("messages deserialise from JSON");
    NCL_CHECK_EQ_INT(ncl_msg_type_from_topic("Query/Response/V1"), NCL_MSG_QUERY_RESPONSE);
    NCL_CHECK_EQ_INT(ncl_msg_type_from_topic("Query/Request/V1"), NCL_MSG_QUERY_REQUEST);
    NCL_CHECK_EQ_INT(ncl_msg_type_from_topic("Probe/Set/Response/V1"),
                     NCL_MSG_PROBE_SET_RESPONSE);
    NCL_CHECK_EQ_INT(ncl_msg_type_from_topic("nonsense"), NCL_MSG_UNKNOWN);

    msg = ncl_message_parse("Query/Response/V1", payload, strlen(payload));
    NCL_CHECK(msg != NULL);
    if (msg != NULL) {
        NCL_CHECK_EQ_INT(msg->type, NCL_MSG_QUERY_RESPONSE);
        NCL_CHECK_EQ_STR(msg->message_id, "m1");
        NCL_CHECK_EQ_INT(ncl_message_item_count(msg), 1);
        item = (ncl_query_response_item *)ncl_message_item_at(msg, 0);
        NCL_CHECK_EQ_STR(item->id, "/STATUS");
        NCL_CHECK(ncl_message_has_data(msg));
        NCL_CHECK(ncl_message_get_data(msg) != NULL);
        ncl_message_free(msg);
    }

    NCL_TEST_CASE("an empty payload is treated as {}");
    msg = ncl_message_parse("Probe/Query/Request/V1", "", 0);
    NCL_CHECK(msg != NULL);
    ncl_message_free(msg);
}

static void test_match(void)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_message *response = ncl_message_new(NCL_MSG_QUERY_RESPONSE);
    ncl_query_response_item *item;

    ncl_message_set_message_id(request, "m1");
    ncl_message_add_query_request_item(request, ncl_query_request_item_new("/STATUS"));

    ncl_message_set_message_id(response, "m1");
    item = ncl_query_response_item_new("/STATUS");
    item->code = ncl_strdup("OK");
    ncl_message_add_query_response_item(response, item);

    NCL_TEST_CASE("responses match their request by @id");
    NCL_CHECK(ncl_message_matches(response, request));
    ncl_message_set_message_id(response, "other");
    NCL_CHECK(!ncl_message_matches(response, request));

    NCL_TEST_CASE("item matching follows the response rules");
    {
        ncl_query_request_item *req = ncl_query_request_item_new("/STATUS");
        ncl_query_response_item *res = ncl_query_response_item_new("/STATUS");
        res->code = ncl_strdup("OK");
        NCL_CHECK(ncl_query_response_item_matches(res, req));

        ncl_query_response_item_free(res);
        res = ncl_query_response_item_new("/OTHER");
        res->code = ncl_strdup("OK");
        NCL_CHECK(!ncl_query_response_item_matches(res, req));

        ncl_params_set_int(&req->params, "offset", 1);
        ncl_query_response_item_free(res);
        res = ncl_query_response_item_new("/STATUS");
        res->code = ncl_strdup("OK");
        NCL_CHECK(!ncl_query_response_item_matches(res, req));
        ncl_params_set_int(&res->params, "offset", 1);
        NCL_CHECK(ncl_query_response_item_matches(res, req));

        ncl_query_response_item_free(res);
        ncl_query_request_item_free(req);
    }
    ncl_message_free(request);
    ncl_message_free(response);
}

static void test_index_expansion(void)
{
    ncl_query_request_item *item = ncl_query_request_item_new("/LIST");
    long long *indexes = NULL;
    size_t count = 0;

    NCL_TEST_CASE("getIndexes() expands \"1-4\" into 1,4");
    ncl_params_append_string(&item->params, "indexes", "3");
    ncl_params_append_string(&item->params, "indexes", "1-4");
    NCL_CHECK_EQ_INT(ncl_query_request_item_indexes(item, &indexes, &count), NCL_OK);
    NCL_CHECK_EQ_INT(count, 3);
    if (count == 3) {
        NCL_CHECK_EQ_INT(indexes[0], 3);
        NCL_CHECK_EQ_INT(indexes[1], 1);
        NCL_CHECK_EQ_INT(indexes[2], 4);
    }
    free(indexes);
    ncl_query_request_item_free(item);
}

NCL_TEST_MAIN_BEGIN()
    test_simple_messages();
    test_query();
    test_set();
    test_sample_and_event();
    test_method_call();
    test_probe_with_model();
    test_validation_and_finalise();
    test_parse_by_topic();
    test_match();
    test_index_expansion();
NCL_TEST_MAIN_END()
