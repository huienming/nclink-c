/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Unit tests for the declaration seam (nclink/ncl_tool.h).
 *
 * The fixture below is written the way an adapter author writes an adapter -
 * one file, a dispatch function, one NCL_TOOL block - and the test drives it
 * through the host's public APIs only: validate, model, register, and a real
 * Query/Set/Method request through the server.
 *
 * The dispatch function is shared by every point on purpose: that is the shape
 * a PLC adapter with a mapping table takes, and it only works if the point's
 * own data (self->arg) and the operation (op) reach it.
 */
#include "ncl_test.h"

#include <string.h>

#include "nclink/ncl_message.h"
#include "nclink/ncl_tool.h"

/* ---------------------------------------------------------------- fixture -- */

/** What a point carries as its own data - here, the "register" it reads. */
typedef struct {
    const char *item;
    long long   value;
} fixture_item;

/* Mutable on purpose: an adapter may park a cache in the point's own data. */
static fixture_item k_run_item = {"STATUS@RUN", 40};

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

static ncl_err fixture_dispatch(void *ctx, const ncl_tool_point *self,
                                ncl_operation op, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    const fixture_item *item = (const fixture_item *)self->arg;

    if (ctx != (void *)&g_run) {
        return ncl_tool_fail(reason, NCL_ERR_STATE,
                             "the handler got a foreign context");
    }
    switch (op) {
    case NCL_OP_GET_VALUE:
        if (item != NULL) {
            /* The point's own data is what makes this a "register" read. */
            return ncl_tool_reply_int(result, item->value + 1);
        }
        if (strcmp(self->path, "/CNC/NAME") == 0) {
            return ncl_tool_reply_text(result, g_name);
        }
        if (strcmp(self->path, "/CNC/MODE") == 0) {
            return ncl_tool_reply_int(result, g_mode);
        }
        return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "no such point %s",
                             self->path);
    case NCL_OP_SET_VALUE:
    {
        const ncl_json *value = ncl_tool_param_value(params);
        long long mode = 0;

        if (value == NULL || !ncl_json_as_int(value, &mode)) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "the write needs an integer value");
        }
        g_written = mode;
        g_mode = mode;
        return ncl_tool_reply_bool(result, true);
    }
    case NCL_OP_FUNC_CALL:
        g_resets++;
        return ncl_tool_reply_bool(result, true);
    default:
        break;
    }
    return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                         "unsupported operation on %s", self->path);
}

NCL_TOOL_BEGIN("cnc", "FANUC 数控机床（夹具）", 1000, 2000,
               fixture_open, fixture_close)
    NCL_POINT_SAMPLED_ARG("/CNC/STATUS@RUN", fixture_dispatch, &k_run_item)
    NCL_POINT("/CNC/NAME", fixture_dispatch)
    NCL_POINT_RW("/CNC/MODE", fixture_dispatch)
    NCL_METHOD("/CNC/RESET", fixture_dispatch)
NCL_TOOL_END()

/** The declaration the macros above built, by value. */
static ncl_tool_decl fixture_decl(void)
{
    return ncl_tool_declaration();
}

/* ------------------------------------------------------------ declaration -- */

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
    NCL_CHECK_EQ_INT(decl.point_count, 4);
    NCL_CHECK(decl.open == fixture_open);
    NCL_CHECK(decl.close == fixture_close);

    NCL_TEST_CASE("a readable point may also be writable and carry its own data");
    NCL_CHECK(decl.points[0].readable);
    NCL_CHECK(decl.points[0].sampled);
    NCL_CHECK(!decl.points[0].writable);
    NCL_CHECK(decl.points[0].arg == (const void *)&k_run_item);
    NCL_CHECK(decl.points[2].readable);
    NCL_CHECK(decl.points[2].writable);
    NCL_CHECK(!decl.points[3].readable);
    NCL_CHECK(decl.points[3].callable);
    NCL_CHECK(decl.points[3].arg == NULL);

    NCL_TEST_CASE("a well formed declaration validates");
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);

    NCL_TEST_CASE("a point answers to the tail of its path");
    NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[0]), "STATUS@RUN");
    NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[3]), "RESET");
    NCL_CHECK_EQ_STR(ncl_tool_point_name(NULL), "");
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

static void test_validate(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_tool_decl broken;
    ncl_tool_point points[2];
    ncl_strbuf err;

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

    NCL_TEST_CASE("path, function and operation have to be there");
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
    points[0] = decl.points[0];
    points[0].readable = false;
    points[0].sampled = false;
    points[0].fn = fixture_dispatch;
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "declares no operation");

    NCL_TEST_CASE("only a readable point may be sampled");
    points[0] = decl.points[0];
    points[0].readable = false;
    points[0].writable = true;
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "sampled but not readable");

    NCL_TEST_CASE("point names have to be unique and usable as method names");
    points[0] = decl.points[0];
    points[1] = decl.points[0];
    points[1].path = "/PLC/STATUS@RUN";
    broken.points = points;
    broken.point_count = 2;
    /* 同一个名字、同一种操作：方法名会撞车，直接拒 */
    expect_refused(&broken, "both declare");
    points[1] = decl.points[0];
    points[1].path = "/CNC/MODE.read";
    expect_refused(&broken, ".read/.write");

    NCL_TEST_CASE("a missing sample period is not an error, it means no channel");
    broken = decl;
    broken.sample_ms = 0;
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(&broken, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    ncl_strbuf_free(&err);
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
    NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 4);
    item = ncl_json_arr_get(items, 0);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "id"), "p0");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "/CNC/STATUS@RUN");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "STATUS");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "number"), "RUN");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "source"), "CNC");
    /* The tail without "@" keeps the path as its type, exactly like the
     * configuration driven model does; a writable point is marked settable. */
    item = ncl_json_arr_get(items, 2);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "/CNC/MODE");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "MODE");
    NCL_CHECK(ncl_json_obj_get_bool(item, "settable", false));
    item = ncl_json_arr_get(items, 1);
    NCL_CHECK(!ncl_json_obj_get_bool(item, "settable", false));

    NCL_TEST_CASE("the sample channel is named after the tool");
    configs = ncl_json_obj_get(node, "configs");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(configs), 1);
    channel = ncl_json_arr_get(configs, 0);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(channel, "id"), "cnc");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(channel, "type"),
                     NCL_NODE_TYPE_SAMPLE_CHANNEL);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(channel, "sampleInterval", 0), 1000);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(channel, "uploadInterval", 0), 2000);
    /* Only the point that asked for it is in the channel. */
    NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(channel, "ids")), 1);
    ncl_json_free(model);

    NCL_TEST_CASE("without a period there is no sample channel");
    quiet = decl;
    quiet.sample_ms = 0;
    model = ncl_tool_model(&quiet, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model != NULL) {
        node = ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(node, "dataItems")),
                         4);
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
    ncl_json *scalar = ncl_json_new_int(9);
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
    NCL_CHECK_EQ_INT(ncl_tool_param_int(NULL, "count", 3), 3);

    NCL_TEST_CASE("the value of a write comes from \"value\" or the params");
    (void)ncl_json_obj_set_int(params, "value", 42);
    NCL_CHECK(ncl_tool_param_value(params) != NULL);
    NCL_CHECK_EQ_INT(ncl_json_as_int(ncl_tool_param_value(params), NULL) ==
                         false,
                     1); /* NULL out pointer is refused, the value is there */
    {
        long long got = 0;

        NCL_CHECK(ncl_json_as_int(ncl_tool_param_value(params), &got));
        NCL_CHECK_EQ_INT(got, 42);
    }
    NCL_CHECK(ncl_tool_param_value(scalar) == scalar);
    NCL_CHECK(ncl_tool_param_value(NULL) == NULL);

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
    ncl_json_free(scalar);
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
    ncl_tool_registration *registration = NULL;

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

    NCL_TEST_CASE("registering a declaration opens once and binds every "
                  "operation");
    NCL_CHECK_EQ_INT(ncl_tool_register(server, &decl, params, &registration,
                                       &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK(registration != NULL);
    NCL_CHECK_EQ_INT(g_opens, 1);
    /* 5 declared operations (read, read, read + write, call): each method is a
     * binding of its own, so methods + "<operation>#<path>" entries = 10. */
    NCL_CHECK_EQ_INT(ncl_server_binding_count(server), 10);
    NCL_CHECK_EQ_INT(ncl_server_operation_count(server), 5);

    NCL_TEST_CASE("a Query reaches the point's function with its own data");
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
                /* The point's arg carried 40, so the value is 41: the handler
                 * read *its own* data, not something it had to guess. */
                NCL_CHECK_EQ_INT(value, 41);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }

    NCL_TEST_CASE("a Query on a point without data still reaches it");
    {
        ncl_message *request = query("/CNC/NAME");
        ncl_message *response = ncl_server_invoke_query(server, request);
        ncl_query_response_item *item;

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
                NCL_CHECK_EQ_STR(ncl_json_as_string(
                                     ncl_query_response_item_data(item)),
                                 "MOCK-1");
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }

    NCL_TEST_CASE("a Set on a readable+writable point carries the value");
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
        ncl_message *request = query("/CNC/MODE");
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

    NCL_TEST_CASE("a Method call is addressed as <tool>/<point name>");
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

    NCL_TEST_CASE("an undeclared method is not found");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response;

        (void)ncl_message_set_method(request, "cnc/NOPE");
        response = ncl_server_invoke_method_call(server, request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_STR(response->as.method_call_response.code,
                             NCL_KW_CODE_NG);
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }

    NCL_TEST_CASE("unregistering closes the connection once");
    ncl_tool_unregister(&decl, registration);
    NCL_CHECK_EQ_INT(g_closes, 1);
    ncl_tool_unregister(&decl, NULL);
    NCL_CHECK_EQ_INT(g_closes, 1);
    ncl_server_free(server);
    ncl_json_free(params);
    ncl_strbuf_free(&err);
}

NCL_TEST_MAIN_BEGIN()
    test_declaration();
    test_validate();
    test_model();
    test_helpers();
    test_register_and_invoke();
NCL_TEST_MAIN_END()
