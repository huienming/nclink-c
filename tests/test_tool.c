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
static int g_last_raw_calls;

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

/** The optional frames callback: the host asks, the module hands bytes over. */
static void fixture_last_raw(void *ctx, ncl_tool_frames *out)
{
    static const unsigned char k_request[] = {0x02, 0x01, 0xa0};
    static const unsigned char k_reply[] = {0x02, 0x01, 0x02, 0x00, 0x07};

    (void)ctx;
    g_last_raw_calls++;
    out->request = k_request;
    out->request_len = sizeof(k_request);
    out->reply = k_reply;
    out->reply_len = sizeof(k_reply);
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
        if (strcmp(self->path, "/MACHINE/NAME") == 0) {
            return ncl_tool_reply_text(result, g_name);
        }
        if (strcmp(self->path, "/MACHINE/MODE") == 0) {
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
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@RUN", fixture_dispatch, &k_run_item)
    NCL_POINT("/MACHINE/NAME", fixture_dispatch)
    NCL_POINT_RW("/MACHINE/MODE", fixture_dispatch)
    NCL_METHOD("/MACHINE/RESET", fixture_dispatch)
NCL_TOOL_END_WITH_RAW(fixture_last_raw)

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
    points[1].path = "/MACHINE/MODE.read";
    expect_refused(&broken, ".read/.write");

    NCL_TEST_CASE("a missing sample period is not an error, it means no channel");
    broken = decl;
    broken.sample_ms = 0;
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(&broken, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    ncl_strbuf_free(&err);

    NCL_TEST_CASE("a point may name itself when its path tail would collide");
    {
        ncl_tool_point named[2];
        ncl_tool_decl with_names = decl;

        named[0] = decl.points[0];
        named[0].path = "/TEST/AXIS@0/POSITION";
        named[0].name = "AXIS0.POSITION";
        named[1] = decl.points[1];
        named[1].path = "/TEST/AXIS@1/POSITION";
        named[1].name = "AXIS1.POSITION";
        with_names.points = named;
        with_names.point_count = 2;
        ncl_strbuf_init(&err);
        NCL_CHECK_EQ_INT(ncl_tool_validate(&with_names, &err), NCL_OK);
        NCL_CHECK_EQ_STR(ncl_tool_point_name(&named[0]), "AXIS0.POSITION");
        NCL_CHECK_EQ_STR(ncl_tool_point_name(&named[1]), "AXIS1.POSITION");
        /* Without the names both tails are "POSITION", and a method name has
         * to be unique: that is the case the field exists for. */
        named[0].name = NULL;
        named[1].name = NULL;
        ncl_strbuf_reset(&err);
        NCL_CHECK_EQ_INT(ncl_tool_validate(&with_names, &err),
                         NCL_ERR_INVALID_ARG);
        /* Both are readable, so the collision is reported as the operation they
         * share rather than as the bare name. */
        NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "both declare") != NULL);
        ncl_strbuf_free(&err);
    }
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
    /* 4 declared points, but /MACHINE/RESET only answers calls: a method is not a
     * data item, so the model carries three. */
    items = ncl_json_obj_get(node, "dataItems");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 3);
    item = ncl_json_arr_get(items, 0);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "id"), "p0");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "/MACHINE/STATUS@RUN");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "STATUS");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "number"), "RUN");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "source"), "MACHINE");
    /* The tail without "@" keeps the path as its type, exactly like the
     * configuration driven model does; a writable point is marked settable. */
    item = ncl_json_arr_get(items, 2);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "/MACHINE/MODE");
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
                         3);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(node, "configs")), 0);
        /* No "device" object: the defaults stand in. */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "type"), "MACHINE");
        ncl_json_free(model);
    }
    ncl_strbuf_free(&err);

    NCL_TEST_CASE("a middle segment becomes a component node (册 32 表 2/表 3)");
    {
        ncl_tool_point nested[2];
        ncl_tool_decl nested_decl = decl;
        ncl_json *model2;
        ncl_json *device_node;
        ncl_json *component;
        ncl_json *component_item;
        ncl_json *direct_item;

        nested[0] = decl.points[0];
        nested[0].path = "/MACHINE/CONTROLLER/PROGRAM";
        nested[1] = decl.points[1];
        nested[1].path = "/MACHINE/STATUS";
        nested_decl.points = nested;
        nested_decl.point_count = 2;
        ncl_strbuf_reset(&err);
        model2 = ncl_tool_model(&nested_decl, NULL, &err);
        NCL_CHECK(model2 != NULL);
        if (model2 != NULL) {
            device_node = ncl_json_arr_get(ncl_json_obj_get(model2, "devices"),
                                           0);
            /* The device node answers on the segment its points use. */
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(device_node, "source"),
                             "MACHINE");
            /* One component, named CONTROLLER, with the PROGRAM under it. */
            {
                ncl_json *components =
                    ncl_json_obj_get(device_node, "components");

                NCL_CHECK_EQ_INT(ncl_json_arr_len(components), 1);
                component = ncl_json_arr_get(components, 0);
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(component, "name"),
                                 "CONTROLLER");
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(component, "type"),
                                 "CONTROLLER");
                component_item =
                    ncl_json_arr_get(ncl_json_obj_get(component, "dataItems"), 0);
                NCL_CHECK_EQ_STR(
                    ncl_json_obj_get_string(component_item, "name"),
                    "/MACHINE/CONTROLLER/PROGRAM");
                NCL_CHECK_EQ_STR(
                    ncl_json_obj_get_string(component_item, "type"), "PROGRAM");
                NCL_CHECK_EQ_STR(
                    ncl_json_obj_get_string(component_item, "source"),
                    "MACHINE/CONTROLLER");
            }
            /* The point without a component still hangs on the device. */
            {
                ncl_json *direct =
                    ncl_json_obj_get(device_node, "dataItems");

                NCL_CHECK_EQ_INT(ncl_json_arr_len(direct), 1);
                direct_item = ncl_json_arr_get(direct, 0);
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(direct_item, "name"),
                                 "/MACHINE/STATUS");
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(direct_item, "source"),
                                 "MACHINE");
            }
            ncl_json_free(model2);
        }
    }
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

/* ------------------------------------------------------------- audit sink -- */

/* The host's §6 trail: the shim reports every point call here. */

static int g_sink_requests;
static int g_sink_writes;
static int g_sink_raw_asked;
static int g_sink_frames;
static char g_sink_path[64];
static ncl_operation g_sink_op;
static int g_sink_code;
static long long g_sink_old = -2;
static long long g_sink_new = -2;

static bool sink_wants_raw(void *user)
{
    (void)user;
    g_sink_raw_asked++;
    return true;
}

static void sink_request(void *user, const char *tool,
                         const ncl_tool_point *point, ncl_operation op, int code,
                         int64_t micros, const ncl_tool_frames *frames)
{
    (void)user;
    (void)tool;
    (void)micros;
    g_sink_requests++;
    g_sink_op = op;
    g_sink_code = code;
    snprintf(g_sink_path, sizeof(g_sink_path), "%s", point->path);
    if (frames != NULL && frames->request != NULL && frames->request_len == 3) {
        if (frames->reply != NULL && frames->reply_len == 5) {
            g_sink_frames++;
        }
    }
}

static void sink_write(void *user, const char *tool, const ncl_tool_point *point,
                       const ncl_json *old_value, const ncl_json *new_value,
                       int code)
{
    (void)user;
    (void)tool;
    (void)point;
    (void)code;
    g_sink_writes++;
    g_sink_old = -1;
    g_sink_new = -1;
    if (old_value != NULL) {
        (void)ncl_json_as_int(old_value, &g_sink_old);
    }
    if (new_value != NULL) {
        (void)ncl_json_as_int(new_value, &g_sink_new);
    }
}

static const ncl_tool_audit k_sink = {
    NULL, sink_wants_raw, sink_request, sink_write,
};

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

/* ---------------------------------------------------------------- pending -- */

/*
 * 待抓包的点位：协议调用还没抓到帧，但架构上已经定下来了。它和普通点位的差别只有一条
 * —— 没有函数可调。模型里有它、客户端问它有明确答复、采样通道里可能占着位置，唯独轮询
 * 不碰它。
 *
 * 自己的声明表，夹具那张表不动，别的用例不受影响。
 */
static const ncl_tool_point k_pending_points[] = {
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS", fixture_dispatch, &k_run_item)
    NCL_POINT_PENDING_SAMPLED("/MACHINE/WARNING", "报警：待抓包（cnc_rdalmmsg2）")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@X/POSITION@CMD",
                            "AXIS_X.POSITION_CMD",
                            "目标位置：待抓包（cnc_rdposition）")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@Y/POSITION@CMD",
                            "AXIS_Y.POSITION_CMD",
                            "目标位置：待抓包（cnc_rdposition）")
};

static ncl_tool_decl pending_decl(void)
{
    ncl_tool_decl decl = fixture_decl();

    decl.points = k_pending_points;
    decl.point_count =
        sizeof(k_pending_points) / sizeof(k_pending_points[0]);
    return decl;
}

static void test_pending(void)
{
    ncl_tool_decl decl = pending_decl();
    ncl_strbuf err;
    ncl_json *model;
    char *model_json;
    ncl_server_options options;
    ncl_server *server;
    ncl_tool_registration *registration = NULL;

    NCL_TEST_CASE("a pending point is still a point: no function, but a reason");
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK(decl.points[1].readable);
    NCL_CHECK(decl.points[1].sampled);
    NCL_CHECK(!decl.points[1].available);
    NCL_CHECK(decl.points[1].fn == NULL);
    NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[1]), "WARNING");
    /* 路径尾段重名的两个目标位置靠显式名字分开，校验也就不会把它们当成撞名。*/
    NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[2]),
                     "AXIS_X.POSITION_CMD");

    NCL_TEST_CASE("a pending point has to say why it cannot be read yet");
    {
        ncl_tool_point points[2];
        ncl_tool_decl broken = decl;

        points[0] = decl.points[0];
        points[1] = decl.points[1];
        points[1].summary = NULL;
        broken.points = points;
        broken.point_count = 2;
        expect_refused(&broken, "needs a summary");
        points[1].summary = "   ";
        expect_refused(&broken, "needs a summary");
    }

    NCL_TEST_CASE("the model carries it with its reason, and the channel keeps "
                  "its place");
    ncl_strbuf_reset(&err);
    model = ncl_tool_model(&decl, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_strbuf_free(&err);
        return;
    }
    {
        ncl_json *node = ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
        ncl_json *items = ncl_json_obj_get(node, "dataItems");
        ncl_json *channel = ncl_json_arr_get(ncl_json_obj_get(node, "configs"), 0);
        ncl_json *ids = ncl_json_obj_get(channel, "ids");
        ncl_json *warning = ncl_json_arr_get(items, 1);

        NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 2);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(warning, "name"),
                         "/MACHINE/WARNING");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(warning, "description"),
                         "报警：待抓包（cnc_rdalmmsg2）");
        /* 默认采样通道：抽样的待抓包点位占着位置（现场要求报警进通道），
         * 没抽样的目标位置不在通道里。 */
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ids), 2);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(ids, 1), "id"),
                         "p1");
    }
    model_json = ncl_json_write_string(model);
    ncl_json_free(model);
    NCL_CHECK(model_json != NULL);

    NCL_TEST_CASE("asking for it answers \"not readable yet\" - not \"no such "
                  "point\" - and does not reach the tool");
    memset(&options, 0, sizeof(options));
    options.sn = "V000000001";
    options.model_json = model_json;
    server = ncl_server_create(&options);
    ncl_free_safe(model_json);
    NCL_CHECK(server != NULL);
    if (server != NULL) {
        ncl_message *request;
        ncl_message *response;
        ncl_query_response_item *item;
        int trail_before = g_sink_requests;

        NCL_CHECK_EQ_INT(ncl_tool_register(server, &decl, NULL, &k_sink,
                                           &registration, &err),
                         NCL_OK);
        request = query("/MACHINE/WARNING");
        response = ncl_server_invoke_query(server, request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_NG);
                NCL_CHECK(item->reason != NULL &&
                          strstr(item->reason, "待抓包") != NULL);
            }
            ncl_message_free(response);
        }
        /* Nothing went over the wire, so the trail has no entry: a request
         * that never happened is not a request. */
        NCL_CHECK_EQ_INT(g_sink_requests, trail_before);
        ncl_tool_unregister(&decl, registration);
        ncl_server_free(server);
    }
    ncl_strbuf_free(&err);
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
    NCL_CHECK_EQ_INT(ncl_tool_register(server, &decl, params, &k_sink, &registration,
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
        ncl_message *request = query("/MACHINE/STATUS@RUN");
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
        ncl_message *request = query("/MACHINE/NAME");
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
        ncl_message *request = set_value("/MACHINE/MODE", 42);
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
        ncl_message *request = query("/MACHINE/MODE");
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

    NCL_TEST_CASE("the host kept the trail for every point call (§6)");
    /* Five calls went through the shim: two reads, the write, the read back
     * and the method call. The last one is the method. */
    NCL_CHECK_EQ_INT(g_sink_requests, 5);
    NCL_CHECK_EQ_INT(g_sink_op, NCL_OP_FUNC_CALL);
    NCL_CHECK_EQ_INT(g_sink_code, NCL_OK);
    NCL_CHECK_EQ_INT(g_sink_writes, 1);
    NCL_CHECK_EQ_INT(g_sink_old, 0);  /* the fixture's mode starts at zero */
    NCL_CHECK_EQ_INT(g_sink_new, 42); /* what the request carried */
    /* The trail asked for frames, so the module's callback ran and its bytes
     * reached the sink. */
    NCL_CHECK(g_sink_raw_asked > 0);
    NCL_CHECK(g_last_raw_calls > 0);
    NCL_CHECK(g_sink_frames > 0);

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
    test_pending();
NCL_TEST_MAIN_END()
