/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 绑定：client 的语义函数直接绑到模型路径上（见 nclink/ncl_tool.h 里"绑取值/置值函数"那一节）。
 *
 * 夹具装作一台 client：那个结构体就是"连接与会话"，函数名就是它读回来的东西。
 * 测试只走宿主的公开 API（模型 → 注册 → Query/Set/Method），验证：
 *   - 数值 / 浮点 / 布尔 / 文本四种取值，以及带现场参数的取值（轴号）；
 *   - 读写点能写回，只读点被写是 NG；
 *   - 语义函数自己报错 → NG + 理由里带错误名；
 *   - 绑定与覆盖混在同一张表里，覆盖那条拿到的还是同一个实例。
 */
#include "ncl_test.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_tool.h"

/* ---------------------------------------------------------------- fixture -- */

enum { AXIS_X = 0, AXIS_Y = 1, AXIS_COUNT = 2 };

/** 一台假 client：结构体是会话，下面那些函数是它的语义 API。 */
typedef struct {
    long long part_count;
    double    position[AXIS_COUNT];
    double    speed[AXIS_COUNT];
    bool      ready;
    long long feed;
    long long spindle_limit; /**< 一个"配置"：参数，不是感知量 */
    char      program[16];
    int       resets;
} fixture_client;

static fixture_client g_client;
static int g_opens;
static int g_closes;

static void *client_open(const ncl_json *params, char **err)
{
    (void)params;
    (void)err;
    g_opens++;
    memset(&g_client, 0, sizeof(g_client));
    g_client.part_count = 1234;
    g_client.position[AXIS_X] = 12.5;
    g_client.position[AXIS_Y] = -3.25;
    g_client.speed[AXIS_X] = 3600.0;
    g_client.ready = true;
    g_client.feed = 100;
    g_client.spindle_limit = 8000;
    snprintf(g_client.program, sizeof(g_client.program), "O1234");
    return &g_client; /* 实例：绑定函数的第一个参数就是它 */
}

static void client_close(void *ctx)
{
    if (ctx != NULL) {
        g_closes++;
    }
}

/* 语义函数：签名按族固定，名字就是它返回的那个量。 */

static ncl_err client_part_count(void *instance, long long *value)
{
    *value = ((fixture_client *)instance)->part_count;
    return NCL_OK;
}

static ncl_err client_axis_position(void *instance, long long axis,
                                    double *value)
{
    if (axis < 0 || axis >= AXIS_COUNT) {
        return NCL_ERR_RANGE;
    }
    *value = ((fixture_client *)instance)->position[axis];
    return NCL_OK;
}

static ncl_err client_axis_speed(void *instance, long long axis, double *value)
{
    if (axis < 0 || axis >= AXIS_COUNT) {
        return NCL_ERR_RANGE;
    }
    *value = ((fixture_client *)instance)->speed[axis];
    return NCL_OK;
}

static ncl_err client_ready(void *instance, bool *value)
{
    *value = ((fixture_client *)instance)->ready;
    return NCL_OK;
}

static ncl_err client_program(void *instance, char *out, size_t cap)
{
    const char *program = ((fixture_client *)instance)->program;
    size_t len = strlen(program);

    if (len + 1 > cap) {
        len = cap - 1;
    }
    memcpy(out, program, len);
    out[len] = '\0';
    return NCL_OK;
}

static ncl_err client_feed_get(void *instance, long long *value)
{
    *value = ((fixture_client *)instance)->feed;
    return NCL_OK;
}

static ncl_err client_feed_set(void *instance, long long value)
{
    ((fixture_client *)instance)->feed = value;
    return NCL_OK;
}

static ncl_err client_spindle_limit_get(void *instance, long long *value)
{
    *value = ((fixture_client *)instance)->spindle_limit;
    return NCL_OK;
}

static ncl_err client_spindle_limit_set(void *instance, long long value)
{
    ((fixture_client *)instance)->spindle_limit = value;
    return NCL_OK;
}

/** 一次"读失败"的语义函数：驱动层断线时就是这个样子。 */
static ncl_err client_broken(void *instance, long long *value)
{
    (void)instance;
    (void)value;
    return NCL_ERR_CLOSED;
}

static ncl_err client_reset(void *instance, const ncl_json *params,
                           ncl_json **result, char **reason)
{
    (void)params;
    (void)reason;
    ((fixture_client *)instance)->resets++;
    return ncl_tool_reply_text(result, "reset done");
}

/* 覆盖档：自己写 dispatch，但拿到的还是同一个实例。 */
static ncl_err coverage_dispatch(void *ctx, const ncl_tool_point *self,
                                 ncl_operation op, const ncl_json *params,
                                 ncl_json **result, char **reason)
{
    const fixture_client *client = (const fixture_client *)ctx;

    (void)op;
    (void)params;
    if (self == NULL || client == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_STATE, "覆盖档没拿到实例");
    }
    /* 状态是推导出来的：这里由 ready + 件数凑一个三态，正是"需要自己解释"的点。 */
    return ncl_tool_reply_text(result, client->ready ? "running" : "free");

}

/*
 * 一张表里混着绑定与覆盖 —— 这是这一层的用法：能用现成的语义函数就绑，需要自己
 * 解释的那一条才写函数。
 */
NCL_TOOL_BEGIN("cnc", "绑定夹具", "MACHINE", 1000, 1000, client_open, client_close)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", client_part_count)
    NCL_DATAITEM_F64_SAMPLED("/AXIS@X/POSITION@REAL",
                             client_axis_position, AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/POSITION@REAL", client_axis_position,
                     AXIS_Y)
    NCL_DATAITEM_F64_SAMPLED("/AXIS@X/SPEED", client_axis_speed, AXIS_X)
    NCL_DATAITEM_BOOL("/READY", client_ready)
    NCL_DATAITEM_STR("/CONTROLLER/PROGRAM", client_program)
    NCL_DATAITEM_I64_RW("/FEED_OVERRIDE", client_feed_get, client_feed_set)
    NCL_DATAITEM_I64("/BROKEN", client_broken)
    /* 配置型数据对象：参数（不是感知量），所以进模型的 configs；不许采样。 */
    NCL_CONFIG_I64_RW("/CONTROLLER/PARAMETER@1",
                           client_spindle_limit_get, client_spindle_limit_set)
    NCL_METHOD_CALL("/RESET", client_reset)
    NCL_DATAITEM_SAMPLED("/STATUS", coverage_dispatch, NULL)
NCL_TOOL_END()

/* ------------------------------------------------------------------ harness -- */

static ncl_message *query(const char *path)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", "get_value");
    (void)ncl_message_set_message_id(request, "q1");
    (void)ncl_message_add_query_request_item(request, item);
    return request;
}

static ncl_message *write_int(const char *path, long long value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", "set_value");
    (void)ncl_params_set_int(&item->params, "value", value);
    (void)ncl_message_set_message_id(request, "s1");
    (void)ncl_message_add_set_request_item(request, item);
    return request;
}

/*
 * 应答里的字符串与数据都活在报文里，报文一释放就没了。所以这些助手在释放之前
 * 把要断言的东西**拷出来**——这是测试自己的纪律，不是库的 API 约定。
 */
static void copy_text(char *out, size_t cap, const char *text)
{
    if (out == NULL || cap == 0) {
        return;
    }
    snprintf(out, cap, "%s", text != NULL ? text : "");
}

/** 读一条路径：*out 收到值，返回 true 表示 code=OK。 */
static bool read_i64(ncl_server *server, const char *path, long long *out)
{
    ncl_message *request = query(path);
    ncl_message *response = ncl_server_invoke_query(server, request);
    bool ok = false;

    if (response != NULL) {
        ncl_query_response_item *item =
            (ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (item != NULL && item->code != NULL &&
            strcmp(item->code, NCL_KW_CODE_OK) == 0) {
            ok = ncl_json_as_int(ncl_query_response_item_data(item), out);
        }
    }
    ncl_message_free(request);
    ncl_message_free(response);
    return ok;
}

static bool read_f64(ncl_server *server, const char *path, double *out)
{
    ncl_message *request = query(path);
    ncl_message *response = ncl_server_invoke_query(server, request);
    bool ok = false;

    if (response != NULL) {
        ncl_query_response_item *item =
            (ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (item != NULL && item->code != NULL &&
            strcmp(item->code, NCL_KW_CODE_OK) == 0) {
            ok = ncl_json_as_double(ncl_query_response_item_data(item), out);
        }
    }
    ncl_message_free(request);
    ncl_message_free(response);
    return ok;
}

static bool read_bool(ncl_server *server, const char *path, bool *out)
{
    ncl_message *request = query(path);
    ncl_message *response = ncl_server_invoke_query(server, request);
    bool ok = false;

    if (response != NULL) {
        ncl_query_response_item *item =
            (ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (item != NULL && item->code != NULL &&
            strcmp(item->code, NCL_KW_CODE_OK) == 0) {
            ok = ncl_json_as_bool(ncl_query_response_item_data(item), out);
        }
    }
    ncl_message_free(request);
    ncl_message_free(response);
    return ok;
}

static bool read_text(ncl_server *server, const char *path, char *out,
                      size_t cap)
{
    ncl_message *request = query(path);
    ncl_message *response = ncl_server_invoke_query(server, request);
    bool ok = false;

    if (response != NULL) {
        ncl_query_response_item *item =
            (ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (item != NULL && item->code != NULL &&
            strcmp(item->code, NCL_KW_CODE_OK) == 0) {
            copy_text(out, cap, ncl_json_as_string(ncl_query_response_item_data(item)));
            ok = true;
        }
    }
    ncl_message_free(request);
    ncl_message_free(response);
    return ok;
}

/** 读一条**应该失败**的路径：理由拷进 @p reason，返回 true 表示确实 NG。 */
static bool read_fails(ncl_server *server, const char *path, char *reason,
                       size_t cap)
{
    ncl_message *request = query(path);
    ncl_message *response = ncl_server_invoke_query(server, request);
    bool failed = false;

    if (response != NULL) {
        ncl_query_response_item *item =
            (ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (item != NULL && item->code != NULL) {
            failed = strcmp(item->code, NCL_KW_CODE_OK) != 0;
            copy_text(reason, cap, item->reason);
        }
    }
    ncl_message_free(request);
    ncl_message_free(response);
    return failed;
}

static bool write_ok(ncl_server *server, const char *path, long long value)
{
    ncl_message *request = write_int(path, value);
    ncl_message *response = ncl_server_invoke_set(server, request);
    bool ok = false;

    if (response != NULL) {
        ncl_set_response_item *item =
            (ncl_set_response_item *)ncl_message_item_at(response, 0);

        ok = item != NULL && item->code != NULL &&
             strcmp(item->code, NCL_KW_CODE_OK) == 0;
    }
    ncl_message_free(request);
    ncl_message_free(response);
    return ok;
}

/* -------------------------------------------------------------------- tests -- */

static void test_declaration(void)
{
    ncl_tool_decl decl = ncl_tool_declaration();
    ncl_strbuf err;

    NCL_TEST_CASE("绑定生成的点位带上了正确的操作位与现场参数");
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    ncl_strbuf_free(&err);

    NCL_CHECK_EQ_INT((int)decl.point_count, 11);
    /* 采样是宏名说了算：带 _SAMPLED 的进默认采样通道 */
    NCL_CHECK(decl.points[0].sampled);
    NCL_CHECK(!decl.points[2].sampled);
    /* 读写点两个位都要有：写之前要读旧值（审计），所以只能读的点不存在 */
    NCL_CHECK(ncl_tool_point_handles(&decl.points[6], NCL_OP_GET_VALUE));
    NCL_CHECK(ncl_tool_point_handles(&decl.points[6], NCL_OP_SET_VALUE));
    NCL_CHECK(!ncl_tool_point_handles(&decl.points[0], NCL_OP_SET_VALUE));
    /* 配置型数据对象：进模型的 configs，而且不允许被采样 */
    NCL_CHECK(decl.points[8].config);
    NCL_CHECK(!decl.points[8].sampled);
    NCL_CHECK(ncl_tool_point_handles(&decl.points[8], NCL_OP_SET_VALUE));
    NCL_CHECK(!decl.points[0].config);
    /* 方法不是数据对象 */
    NCL_CHECK(ncl_tool_point_is_method(&decl.points[9]));
    /* 现场参数落在点位的 arg 里，取值函数由宏带进去 */
    {
        const ncl_tool_value_spec *x =
            (const ncl_tool_value_spec *)decl.points[1].arg;
        const ncl_tool_value_spec *y =
            (const ncl_tool_value_spec *)decl.points[2].arg;
        const ncl_tool_value_spec *rw =
            (const ncl_tool_value_spec *)decl.points[6].arg;
        const ncl_tool_value_spec *cfg =
            (const ncl_tool_value_spec *)decl.points[8].arg;

        NCL_CHECK(x != NULL && x->arg == AXIS_X);
        NCL_CHECK(y != NULL && y->arg == AXIS_Y);
        NCL_CHECK(rw != NULL && rw->get != NULL && rw->set != NULL);
        NCL_CHECK(cfg != NULL && cfg->get != NULL && cfg->set != NULL);
    }
}

static void test_read_and_write(void)
{
    ncl_tool_decl decl = ncl_tool_declaration();
    ncl_tool_registration *registration = NULL;
    ncl_server_options options;
    ncl_server *server;
    ncl_strbuf err;
    ncl_json *model;
    char *model_json;
    long long number = 0;
    double real = 0.0;
    bool flag = false;
    char text[64];
    char reason[256];

    ncl_strbuf_init(&err);
    model = ncl_tool_model(&decl, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_strbuf_free(&err);
        return;
    }
    model_json = ncl_json_write_string(model);
    ncl_json_free(model);

    memset(&options, 0, sizeof(options));
    options.sn = "V000000001";
    options.model_json = model_json;
    server = ncl_server_create(&options);
    ncl_free_safe(model_json);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        ncl_strbuf_free(&err);
        return;
    }
    NCL_CHECK_EQ_INT(ncl_tool_register(server, &decl, NULL, NULL, &registration,
                                       &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT(g_opens, 1);

    NCL_TEST_CASE("整数、浮点、布尔、文本四种绑定都读得回来");
    NCL_CHECK(read_i64(server, "/MACHINE/PART_COUNT", &number));
    NCL_CHECK_EQ_INT(number, 1234);
    NCL_CHECK(read_f64(server, "/MACHINE/AXIS@X/POSITION@REAL", &real));
    NCL_CHECK(real > 12.49 && real < 12.51);
    NCL_CHECK(read_bool(server, "/MACHINE/READY", &flag));
    NCL_CHECK(flag);

    NCL_TEST_CASE("带现场参数的两条绑定各自拿到自己的轴");
    NCL_CHECK(read_f64(server, "/MACHINE/AXIS@Y/POSITION@REAL", &real));
    NCL_CHECK(real < -3.24 && real > -3.26);
    NCL_CHECK(read_f64(server, "/MACHINE/AXIS@X/SPEED", &real));
    NCL_CHECK(real > 3599.9);

    NCL_TEST_CASE("文本绑定把 client 的字符串原样交出去");
    NCL_CHECK(read_text(server, "/MACHINE/CONTROLLER/PROGRAM", text,
                        sizeof(text)));
    NCL_CHECK_EQ_STR(text, "O1234");

    NCL_TEST_CASE("读写点写回并读得到新值");
    NCL_CHECK(write_ok(server, "/MACHINE/FEED_OVERRIDE", 42));
    NCL_CHECK_EQ_INT(g_client.feed, 42);
    NCL_CHECK(read_i64(server, "/MACHINE/FEED_OVERRIDE", &number));
    NCL_CHECK_EQ_INT(number, 42);

    NCL_TEST_CASE("只读点被写是 NG（点位没声明 set_value）");
    NCL_CHECK(!write_ok(server, "/MACHINE/PART_COUNT", 7));

    NCL_TEST_CASE("语义函数自己报错 → NG，理由里带路径与错误名");
    memset(reason, 0, sizeof(reason));
    NCL_CHECK(read_fails(server, "/MACHINE/BROKEN", reason, sizeof(reason)));
    NCL_CHECK(strstr(reason, "BROKEN") != NULL);
    NCL_CHECK(strstr(reason, ncl_err_name(NCL_ERR_CLOSED)) != NULL);

    NCL_TEST_CASE("覆盖档与绑定混用，拿到的还是同一个实例");
    NCL_CHECK(read_text(server, "/MACHINE/STATUS", text, sizeof(text)));
    NCL_CHECK_EQ_STR(text, "running");

    NCL_TEST_CASE("配置型绑定：读得到、写得进，且落在模型的 configs 里");
    NCL_CHECK(read_i64(server, "/MACHINE/CONTROLLER/PARAMETER@1", &number));
    NCL_CHECK_EQ_INT(number, 8000);
    NCL_CHECK(write_ok(server, "/MACHINE/CONTROLLER/PARAMETER@1", 6000));
    NCL_CHECK_EQ_INT(g_client.spindle_limit, 6000);
    {
        ncl_json *model_again = ncl_tool_model(&decl, NULL, NULL);
        ncl_json *device;

        NCL_CHECK(model_again != NULL);
        device = ncl_json_arr_get(ncl_json_obj_get(model_again, "devices"), 0);
        NCL_CHECK(device != NULL);
        if (device != NULL) {
            /*
             * 绑定的配置落进 configs（数据项在不在同一个节点上、按组件怎么分，
             * 是模型写出器的事，test_tool.c 有专门的断言）。
             */
            NCL_CHECK_EQ_INT(
                ncl_json_arr_len(ncl_json_obj_get(device, "configs")), 1);
        }
        ncl_json_free(model_again);
    }

    NCL_TEST_CASE("方法：绑定一行，实例里记下这次调用");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response;

        (void)ncl_message_set_message_id(request, "m1");
        (void)ncl_message_set_method(request, "cnc/RESET");
        response = ncl_server_invoke_method_call(server, request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            NCL_CHECK_EQ_STR(response->as.method_call_response.code,
                             NCL_KW_CODE_OK);
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }
    NCL_CHECK_EQ_INT(g_client.resets, 1);

    ncl_tool_unregister(&decl, registration);
    NCL_CHECK_EQ_INT(g_closes, 1);
    ncl_server_free(server);
    ncl_strbuf_free(&err);
}

NCL_TEST_MAIN_BEGIN()
    test_declaration();
    test_read_and_write();
NCL_TEST_MAIN_END()
