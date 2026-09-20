/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Unit tests for the declaration seam (nclink/ncl_tool.h).
 *
 * The fixture below is written the way an adapter author writes an adapter -
 * one file, a handful of functions, one NCL_TOOL block - and the test then
 * drives it through the host's public APIs only: validate, model, register,
 * and a real Query/Set/Method request through the server. If either half of
 * the seam moves without the other, this suite notices.
 */
#include "ncl_test.h"

#include <string.h>

#include "nclink/ncl_message.h"
#include "nclink/ncl_tool.h"

/* ---------------------------------------------------------------- fixture -- */

static int g_opens;
static int g_closes;
static int g_resets;
static long long g_run = 1;
static long long g_mode;
static long long g_written = -1;
static const char *g_name = "MOCK-1";

static void *fixture_open(const ncl_json *params, char **err)
{
    (void)err;
    g_opens++;
    /* The host hands over the module's "parameters" object, so a default read
     * from it is how a test sees that open() got the configuration. */
    g_run = ncl_tool_param_int(params, "run", g_run);
    return (void *)&g_run; /* the "connection": one int, nothing real behind it */
}

static void fixture_close(void *ctx)
{
    if (ctx != NULL) {
        g_closes++;
    }
}

static ncl_err read_run(void *ctx, const ncl_json *params, ncl_json **result,
                        char **reason)
{
    (void)params;
    if (ctx != (void *)&g_run) {
        return ncl_tool_fail(reason, NCL_ERR_STATE, "read_run got a foreign context");
    }
    return ncl_tool_reply_int(result, g_run);
}

static ncl_err read_name(void *ctx, const ncl_json *params, ncl_json **result,
                         char **reason)
{
    (void)ctx;
    (void)params;
    (void)reason;
    return ncl_tool_reply_text(result, g_name);
}

static ncl_err read_mode(void *ctx, const ncl_json *params, ncl_json **result,
                         char **reason)
{
    (void)ctx;
    (void)params;
    (void)reason;
    return ncl_tool_reply_int(result, g_mode);
}

static ncl_err write_mode(void *ctx, const ncl_json *params, ncl_json **result,
                          char **reason)
{
    long long value = ncl_tool_param_int(params, "value", -1);

    (void)ctx;
    if (value < 0) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                             "the write needs a non negative value");
    }
    g_written = value;
    g_mode = value;
    return ncl_tool_reply_bool(result, true);
}

static ncl_err call_reset(void *ctx, const ncl_json *params, ncl_json **result,
                          char **reason)
{
    (void)ctx;
    (void)params;
    (void)reason;
    g_resets++;
    return ncl_tool_reply_bool(result, true);
}

NCL_TOOL_BEGIN("cnc", "FANUC 数控机床（夹具）", 1000, 2000,
               fixture_open, fixture_close)
    NCL_POINT_SAMPLED("/CNC/STATUS@RUN", read_run)
    NCL_POINT_SAMPLED("/CNC/NAME", read_name)
    NCL_POINT("/CNC/MODE@CUR", read_mode)
    NCL_POINT_WRITE("/CNC/MODE", write_mode)
    NCL_METHOD("/CNC/RESET", call_reset)
NCL_TOOL_END()

/* ------------------------------------------------------------ declaration -- */

/** The declaration the macros above built, by value. */
static ncl_tool_decl fixture_decl(void)
{
    return ncl_tool_declaration();
}
static void test_declaration(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_strbuf err;

    NCL_TEST_CASE("the declaration macros describe the tool");
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_STR(decl.name, "cnc");
    NCL_CHECK_EQ_STR(decl.description, "FANUC 数控机床（夹具）");
    NCL_CHECK_EQ_INT(decl.sample_ms, 1000);
    NCL_CHECK_EQ_INT(decl.upload_ms, 2000);
    NCL_CHECK_EQ_INT(decl.point_count, 5);
    NCL_CHECK(decl.open == fixture_open);
    NCL_CHECK(decl.close == fixture_close);
    NCL_CHECK_EQ_INT(decl.points[0].kind, NCL_TOOL_GET);
    NCL_CHECK(decl.points[0].sampled);
    NCL_CHECK_EQ_STR(decl.points[0].path, "/CNC/STATUS@RUN");
    NCL_CHECK_EQ_INT(decl.points[3].kind, NCL_TOOL_SET);
    NCL_CHECK_EQ_INT(decl.points[4].kind, NCL_TOOL_CALL);
    NCL_CHECK(!decl.points[4].sampled);

    NCL_TEST_CASE("a well formed declaration validates");
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);

    NCL_TEST_CASE("kind to operation/method mapping");
    NCL_CHECK_EQ_INT(ncl_tool_kind_operation(NCL_TOOL_GET), NCL_OP_GET_VALUE);
    NCL_CHECK_EQ_INT(ncl_tool_kind_operation(NCL_TOOL_SET), NCL_OP_SET_VALUE);
    NCL_CHECK_EQ_INT(ncl_tool_kind_operation(NCL_TOOL_CALL), NCL_OP_FUNC_CALL);
    NCL_CHECK_EQ_STR(ncl_tool_kind_method(NCL_TOOL_GET), "read");
    NCL_CHECK_EQ_STR(ncl_tool_kind_method(NCL_TOOL_SET), "write");
    NCL_CHECK_EQ_STR(ncl_tool_kind_method(NCL_TOOL_CALL), "call");
    ncl_strbuf_free(&err);
}

/** Every broken declaration has to be refused with the offending path named. */
static void expect_refused(const ncl_tool_decl *decl, const char *hint)
{
    ncl_strbuf err;

    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(decl, &err), NCL_ERR_INVALID_ARG);
    if (hint != NULL) {
        NCL_CHECK(strstr(ncl_strbuf_cstr(&err), hint) != NULL);
    }
    ncl_strbuf_free(&err);
}

static void test_validate_refusals(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_tool_decl broken;
    ncl_tool_point points[2];

    NCL_TEST_CASE("a tool without a name, an open function or points is refused");
    broken = decl;
    broken.name = "   ";
    expect_refused(&broken, "no name");
    broken = decl;
    broken.open = NULL;
    expect_refused(&broken, "no open function");
    broken = decl;
    broken.points = NULL;
    broken.point_count = 0;
    expect_refused(&broken, "declares no point");

    NCL_TEST_CASE("a path that is not absolute and a point without a function");
    points[0] = decl.points[0];
    points[0].path = "CNC/STATUS";
    points[1] = decl.points[1];
    broken = decl;
    broken.points = points;
    broken.point_count = 2;
    expect_refused(&broken, "must start with '/'");
    points[0] = decl.points[0];
    points[0].fn = NULL;
    expect_refused(&broken, "has no function");

    NCL_TEST_CASE("the same path may not be declared twice");
    points[0] = decl.points[0];
    points[1] = decl.points[0];
    expect_refused(&broken, "declared twice");

    NCL_TEST_CASE("only readable points may be sampled");
    points[0] = decl.points[3]; /* the writable one */
    points[0].sampled = true;
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "sampled but not readable");

    NCL_TEST_CASE("sampled points need a period");
    points[0] = decl.points[0];
    broken.points = points;
    broken.point_count = 1;
    broken.sample_ms = 0;
    expect_refused(&broken, "declares no period");
}

/* ------------------------------------------------------------------ model -- */

static void test_model(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_strbuf err;
    ncl_json *device;
    ncl_json *model;
    ncl_json *node;
    ncl_json *items;
    ncl_json *configs;
    ncl_json *channel;
    ncl_json *item;
    ncl_tool_decl quiet;
    ncl_tool_point points[3];

    ncl_strbuf_init(&err);
    device = ncl_json_new_object();
    NCL_CHECK(device != NULL);
    (void)ncl_json_obj_set_string(device, "type", "CNC");
    (void)ncl_json_obj_set_string(device, "id", "V9");
    (void)ncl_json_obj_set_string(device, "name", "夹具机床");

    model = ncl_tool_model(&decl, device, &err);
    ncl_json_free(device);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_strbuf_free(&err);
        return;
    }

    NCL_TEST_CASE("the model carries one data item per declared point");
    node = ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
    NCL_CHECK(node != NULL);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "type"), "CNC");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "id"), "V9");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "name"), "夹具机床");
    items = ncl_json_obj_get(node, "dataItems");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 5);
    item = ncl_json_arr_get(items, 0);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "id"), "p0");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "/CNC/STATUS@RUN");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "STATUS");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "number"), "RUN");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "source"), "CNC");
    /* The tail without "@" keeps the path as its type, exactly like the
     * configuration driven model does. */
    item = ncl_json_arr_get(items, 3);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "/CNC/MODE");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "MODE");

    NCL_TEST_CASE("the sample channel is named after the tool");
    configs = ncl_json_obj_get(node, "configs");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(configs), 1);
    channel = ncl_json_arr_get(configs, 0);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(channel, "id"), "cnc");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(channel, "type"),
                     NCL_NODE_TYPE_SAMPLE_CHANNEL);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(channel, "sampleInterval", 0), 1000);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(channel, "uploadInterval", 0), 2000);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(channel, "ids")), 2);
    ncl_json_free(model);

    NCL_TEST_CASE("a tool without sampled points gets no sample channel");
    points[0] = decl.points[2];
    points[1] = decl.points[3];
    points[2] = decl.points[4];
    quiet = decl;
    quiet.points = points;
    quiet.point_count = 3;
    model = ncl_tool_model(&quiet, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model != NULL) {
        node = ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(node, "dataItems")), 3);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(node, "configs")), 0);
        /* No "device" object: the defaults stand in. */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "type"), "MACHINE");
        ncl_json_free(model);
    }
    ncl_strbuf_free(&err);
}

/* ---------------------------------------------------------------- helpers -- */

static void test_helpers(void)
{
    ncl_json *params = ncl_json_new_object();
    ncl_json *value = NULL;
    char *reason = NULL;

    NCL_TEST_CASE("parameter helpers fall back when the key is absent");
    (void)ncl_json_obj_set_int(params, "count", 7);
    (void)ncl_json_obj_set_string(params, "host", "10.0.0.1");
    (void)ncl_json_obj_set_bool(params, "fast", true);
    NCL_CHECK_EQ_INT(ncl_tool_param_int(params, "count", 0), 7);
    NCL_CHECK_EQ_INT(ncl_tool_param_int(params, "missing", 5), 5);
    NCL_CHECK_EQ_STR(ncl_tool_param_str(params, "host", ""), "10.0.0.1");
    NCL_CHECK_EQ_STR(ncl_tool_param_str(params, "missing", "fallback"),
                     "fallback");
    NCL_CHECK(ncl_tool_param_bool(params, "fast", false));
    NCL_CHECK(ncl_tool_param_bool(params, "missing", true));
    /* A NULL params object is the normal "no parameters in the configuration"
     * case, not an error. */
    NCL_CHECK_EQ_INT(ncl_tool_param_int(NULL, "count", 3), 3);
    NCL_CHECK_EQ_STR(ncl_tool_param_str(NULL, "host", "x"), "x");

    NCL_TEST_CASE("reply helpers build the value the point returns");
    NCL_CHECK_EQ_INT(ncl_tool_reply_int(&value, 42), NCL_OK);
    {
        long long got = 0;

        NCL_CHECK(value != NULL);
        NCL_CHECK_EQ_INT(ncl_json_as_int(value, &got), true);
        NCL_CHECK_EQ_INT(got, 42);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(ncl_tool_reply_text(&value, "hello"), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "hello");
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(ncl_tool_reply_bool(&value, true), NCL_OK);
    NCL_CHECK_EQ_INT(value != NULL, 1);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(ncl_tool_reply_text(&value, NULL), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "");
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(ncl_tool_reply_int(NULL, 1), NCL_ERR_INVALID_ARG);

    NCL_TEST_CASE("fail() carries the code and the message a client sees");
    NCL_CHECK_EQ_INT(ncl_tool_fail(&reason, NCL_ERR_TIMEOUT,
                                   "no answer from %s", "10.0.0.1:8193"),
                     NCL_ERR_TIMEOUT);
    NCL_CHECK_EQ_STR(reason, "no answer from 10.0.0.1:8193");
    ncl_free_safe(reason);
    NCL_CHECK_EQ_INT(ncl_tool_fail(NULL, NCL_ERR_IO, "ignored"), NCL_ERR_IO);
    ncl_json_free(params);
}

/* -------------------------------------------------------- register/invoke -- */

static ncl_message *query(const char *path)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", "get_value");
    (void)ncl_message_set_message_id(request, "q1");
    (void)ncl_message_add_query_request_item(request, item);
    return request;
}

static ncl_message *set_value(const char *path, long long value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", "set_value");
    (void)ncl_params_set_int(&item->params, "value", value);
    (void)ncl_message_set_message_id(request, "s1");
    (void)ncl_message_add_set_request_item(request, item);
    return request;
}

static void test_register_and_invoke(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_strbuf err;
    ncl_json *params;
    ncl_json *model;
    ncl_json *device = ncl_json_new_object();
    char *model_json;
    ncl_server_options options;
    ncl_server *server;
    void *ctx = NULL;


    ncl_strbuf_init(&err);
    params = ncl_json_new_object();
    (void)ncl_json_obj_set_int(params, "run", 7);
    model = ncl_tool_model(&decl, device, &err);
    ncl_json_free(device);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_json_free(params);
        ncl_strbuf_free(&err);
        return;
    }
    model_json = ncl_json_write_string(model);
    ncl_json_free(model);
    NCL_CHECK(model_json != NULL);

    memset(&options, 0, sizeof(options));
    options.sn = "V000000001";
    options.model_json = model_json;
    server = ncl_server_create(&options);
    ncl_free_safe(model_json);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        ncl_json_free(params);
        ncl_strbuf_free(&err);
        return;
    }

    NCL_TEST_CASE("registering a declaration opens once and binds every point");
    NCL_CHECK_EQ_INT(ncl_tool_register(server, &decl, params, &ctx, &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK(ctx == (void *)&g_run);
    NCL_CHECK_EQ_INT(g_opens, 1);
    /* Every method is a binding of its own, so the count is methods plus
     * "<operation>#<path>" entries: 5 + 5. */
    NCL_CHECK_EQ_INT(ncl_server_binding_count(server), 10);
    NCL_CHECK_EQ_INT(ncl_server_operation_count(server), 5);

    NCL_TEST_CASE("a Query request reaches the declared read function");
    {
        ncl_message *request = query("/CNC/STATUS@RUN");
        ncl_message *response = ncl_server_invoke_query(server, request);
        ncl_query_response_item *item;

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            long long value = 0;

            item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
                NCL_CHECK(ncl_json_as_int(ncl_query_response_item_data(item),
                                          &value));
                NCL_CHECK_EQ_INT(value, 7);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }

    NCL_TEST_CASE("a Set request reaches the declared write function");
    {
        ncl_message *request = set_value("/CNC/MODE", 42);
        ncl_message *response = ncl_server_invoke_set(server, request);
        ncl_set_response_item *item;

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            item = (ncl_set_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
        NCL_CHECK_EQ_INT(g_written, 42);
    }

    NCL_TEST_CASE("the written value is what the next read reports");
    {
        ncl_message *request = query("/CNC/MODE@CUR");
        ncl_message *response = ncl_server_invoke_query(server, request);
        ncl_query_response_item *item;
        long long value = 0;

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
                NCL_CHECK(ncl_json_as_int(ncl_query_response_item_data(item),
                                          &value));
                NCL_CHECK_EQ_INT(value, 42);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }

    NCL_TEST_CASE("a Method call reaches the declared method function");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response;

        (void)ncl_message_set_message_id(request, "m1");
        (void)ncl_message_set_method(request, "cnc/RESET");
        response = ncl_server_invoke_method_call(server, request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_INT(response->type, NCL_MSG_METHOD_CALL_RESPONSE);
            NCL_CHECK_EQ_STR(response->as.method_call_response.code,
                             NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
        ncl_message_free(request);
        NCL_CHECK_EQ_INT(g_resets, 1);
    }

    NCL_TEST_CASE("closing the tool runs close() exactly once");
    ncl_tool_close(&decl, ctx);
    NCL_CHECK_EQ_INT(g_closes, 1);
    ncl_tool_close(NULL, ctx);
    NCL_CHECK_EQ_INT(g_closes, 1);
    ncl_server_free(server);
    ncl_json_free(params);
    ncl_strbuf_free(&err);
}

NCL_TEST_MAIN_BEGIN()
    test_declaration();
    test_validate_refusals();
    test_model();
    test_helpers();
    test_register_and_invoke();
NCL_TEST_MAIN_END()
