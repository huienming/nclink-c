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
#include "nclink/ncl_model.h"
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

NCL_TOOL_BEGIN("cnc", "FANUC 数控机床（夹具）", "MACHINE", 1000, 2000,
               fixture_open, fixture_close)
    NCL_DATAITEM_SAMPLED("/STATUS@RUN", fixture_dispatch, &k_run_item)
    /* 表 6 的 NAME 属"不常变"的元信息：进模型的 configs，不进采样通道。 */
    NCL_CONFIG("/NAME", fixture_dispatch, NULL)
    NCL_DATAITEM_RW("/MODE", fixture_dispatch, NULL)
    NCL_METHOD("/RESET", fixture_dispatch, NULL)
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
    NCL_CHECK(ncl_tool_point_handles(&decl.points[0], NCL_OP_GET_VALUE));
    NCL_CHECK(decl.points[0].sampled);
    NCL_CHECK(!ncl_tool_point_handles(&decl.points[0], NCL_OP_SET_VALUE));
    NCL_CHECK(decl.points[0].arg == (const void *)&k_run_item);
    NCL_CHECK(ncl_tool_point_handles(&decl.points[2], NCL_OP_GET_VALUE));
    NCL_CHECK(ncl_tool_point_handles(&decl.points[2], NCL_OP_SET_VALUE));
    NCL_CHECK(!ncl_tool_point_handles(&decl.points[3], NCL_OP_GET_VALUE));
    NCL_CHECK(ncl_tool_point_is_method(&decl.points[3]));
    NCL_CHECK(ncl_tool_point_handles(&decl.points[3], NCL_OP_FUNC_CALL));
    NCL_CHECK(decl.points[3].arg == NULL);

    NCL_TEST_CASE("a well formed declaration validates");
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);

    NCL_TEST_CASE("a point name comes from its path（去掉设备段，'@'→'_'，'/'→'.'）");
    {
        char name[256];

        NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[0], name, sizeof(name)),
                         "STATUS_RUN");
        NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[3], name, sizeof(name)),
                         "RESET");
        NCL_CHECK_EQ_STR(ncl_tool_point_name(NULL, name, sizeof(name)), "");
    }
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
    points[0].ops = 0; /* no operation at all */
    points[0].sampled = false;
    points[0].fn = fixture_dispatch;
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "declares no operation");

    NCL_TEST_CASE("only a readable point may be sampled");
    points[0] = decl.points[0];
    points[0].ops = NCL_OP_BIT(NCL_OP_SET_VALUE);
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "sampled but not readable");

    NCL_TEST_CASE("a point that can be written can always be read too");
    points[0] = decl.points[0];
    points[0].sampled = false;
    points[0].ops = NCL_OP_BIT(NCL_OP_SET_VALUE);
    points[0].fn = fixture_dispatch;
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "can be written but not read");

    NCL_TEST_CASE("point names have to be unique and usable as method names");
    points[0] = decl.points[0];
    points[1] = decl.points[0];
    broken.points = points;
    broken.point_count = 2;
    /* 同一条路径声明两次：绑定键与方法名都会撞，直接拒 */
    expect_refused(&broken, "both declare");
    /* 两条不同路径也可能推出同一个名字：'@' 换成 '_'，所以 /AXIS@X/POSITION 与
     * /AXIS_X/POSITION 都叫 AXIS_X.POSITION —— 一样拒。 */
    points[1].path = "/AXIS_X/POSITION";
    points[0] = decl.points[2];
    points[0].path = "/AXIS@X/POSITION";
    expect_refused(&broken, "both declare");
    points[0] = decl.points[0];
    points[1] = decl.points[0];
    points[1].path = "/MODE.read";
    expect_refused(&broken, ".read/.write");

    NCL_TEST_CASE("a missing sample period is not an error, it means no channel");
    broken = decl;
    broken.sample_ms = 0;
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(&broken, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    ncl_strbuf_free(&err);

    NCL_TEST_CASE("配置型数据（参数、坐标系、刀具表…）不能进采样通道");
    points[0] = decl.points[1]; /* /MACHINE/NAME：表 6 的元信息，属 configs */
    points[0].sampled = true;
    points[0].fn = fixture_dispatch;
    broken = decl;
    broken.points = points;
    broken.point_count = 1;
    expect_refused(&broken, "配置型数据不得作为采样数据源");

    NCL_TEST_CASE("同名尾段不会撞名：名字从整条路径推");
    {
        ncl_tool_point named[2];
        ncl_tool_decl nested = decl;
        char name[256];

        named[0] = decl.points[0];
        named[0].path = "/AXIS@0/POSITION";
        named[1] = decl.points[1];
        named[1].path = "/AXIS@1/POSITION";
        nested.points = named;
        nested.point_count = 2;
        ncl_strbuf_init(&err);
        NCL_CHECK_EQ_INT(ncl_tool_validate(&nested, &err), NCL_OK);
        NCL_CHECK_EQ_STR(ncl_tool_point_name(&named[0], name, sizeof(name)),
                         "AXIS_0.POSITION");
        NCL_CHECK_EQ_STR(ncl_tool_point_name(&named[1], name, sizeof(name)),
                         "AXIS_1.POSITION");
        /* 两条路径推不出同一个名字，所以尾段重名不再是问题；真撞了（比如分段里
         * 有点号）还是会被拒。 */
        named[1].path = "/AXIS@0/POSITION"; /* 同一条路径，声明两次 */
        ncl_strbuf_reset(&err);
        NCL_CHECK_EQ_INT(ncl_tool_validate(&nested, &err), NCL_ERR_INVALID_ARG);
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
    (void)ncl_json_obj_set_string(device, "type", "MACHINE");
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
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "type"), "MACHINE");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "id"), "V9");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(node, "name"), "夹具机床");
    /* 4 declared points: /MACHINE/RESET only answers calls (a method is not a data
     * item) and /MACHINE/NAME is a 表 6 metadata type, so it goes to configs
     * (册 3 表 1 注 b：配置型数据对象不是采样源)。剩下的才是 dataItems。 */
    items = ncl_json_obj_get(node, "dataItems");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 2);
    item = ncl_json_arr_get(items, 0);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "id"), "p0");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "运行状态（RUN）");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "STATUS");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "number"), "RUN");
    /* 没有 source：模型树自己就能走出一条与我们声明的路径一模一样的路径，
     * 所以这里不留"另一条路"，免得两个算法给出两个路径。 */
    NCL_CHECK(ncl_json_obj_get(item, "source") == NULL);
    /* The tail without "@" keeps the path as its type; a type the dictionary
     * does not know keeps itself as the name (no label to translate to). */
    item = ncl_json_arr_get(items, 1);
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "MODE");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "MODE");
    NCL_CHECK(ncl_json_obj_get_bool(item, "settable", false));
    NCL_CHECK(!ncl_json_obj_get_bool(ncl_json_arr_get(items, 0), "settable",
                                     false));

    NCL_TEST_CASE("配置型数据对象进 configs，采样通道也在 configs 里");
    configs = ncl_json_obj_get(node, "configs");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(configs), 2);
    item = ncl_json_arr_get(configs, 0); /* 配置型数据排在采样通道前面 */
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "id"), "p1");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "name"), "名称");
    NCL_CHECK_EQ_STR(ncl_json_obj_get_string(item, "type"), "NAME");
    channel = ncl_json_arr_get(configs, 1);
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
                         2);
        /* 没有采样通道了，但 /MACHINE/NAME 这个配置型数据还在。 */
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(node, "configs")), 1);
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
        nested[0].path = "/CONTROLLER/PROGRAM";
        nested[1] = decl.points[2]; /* a data item (points[1] 是 config) */
        nested[1].path = "/STATUS";
        nested_decl.points = nested;
        nested_decl.point_count = 2;
        ncl_strbuf_reset(&err);
        model2 = ncl_tool_model(&nested_decl, NULL, &err);
        NCL_CHECK(model2 != NULL);
        if (model2 != NULL) {
            device_node = ncl_json_arr_get(ncl_json_obj_get(model2, "devices"),
                                           0);
            /* The device node answers on the segment its points use. */
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(device_node, "type"),
                             "MACHINE");
            NCL_CHECK(ncl_json_obj_get(device_node, "source") == NULL);
            /* One component, named CONTROLLER, with the PROGRAM under it. */
            {
                ncl_json *components =
                    ncl_json_obj_get(device_node, "components");

                NCL_CHECK_EQ_INT(ncl_json_arr_len(components), 1);
                component = ncl_json_arr_get(components, 0);
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(component, "name"),
                                 "控制器");
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(component, "type"),
                                 "CONTROLLER");
                component_item =
                    ncl_json_arr_get(ncl_json_obj_get(component, "dataItems"), 0);
                NCL_CHECK_EQ_STR(
                    ncl_json_obj_get_string(component_item, "name"),
                    "主程序名");
                NCL_CHECK_EQ_STR(
                    ncl_json_obj_get_string(component_item, "type"), "PROGRAM");
                NCL_CHECK(ncl_json_obj_get(component_item, "source") == NULL);
            }
            /* The point without a component still hangs on the device. */
            {
                ncl_json *direct =
                    ncl_json_obj_get(device_node, "dataItems");

                NCL_CHECK_EQ_INT(ncl_json_arr_len(direct), 1);
                direct_item = ncl_json_arr_get(direct, 0);
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(direct_item, "name"),
                                 "运行状态");
                NCL_CHECK(ncl_json_obj_get(direct_item, "source") == NULL);
            }
            ncl_json_free(model2);
        }
    }
}

/* ---------------------------------------------------------------- helpers -- */

/*
 * 模型树走出来的路径，必须和声明里的路径一模一样 —— 这是"source 与父节点拼接
 * 不能给出两个结果"的另一半：声明里没有 source，所以这里测的就是父节点拼接。
 */
static void test_model_paths_match_declaration(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_strbuf err;
    ncl_json *device;
    ncl_json *model;
    char *text;
    size_t i;

    ncl_strbuf_init(&err);
    device = ncl_json_new_object();
    (void)ncl_json_obj_set_string(device, "type", "MACHINE");
    model = ncl_tool_model(&decl, device, &err);
    ncl_json_free(device);

    NCL_TEST_CASE("模型树走出来的路径 == 声明的路径（父节点拼接与 source 一个结果）");
    NCL_CHECK(model != NULL);
    if (model != NULL) {
        ncl_node *root;
        ncl_node_map paths;

        text = ncl_json_write_string(model);
        ncl_json_free(model);
        NCL_CHECK(text != NULL);
        root = text != NULL ? ncl_root_node_parse(text) : NULL;
        ncl_free_safe(text);
        NCL_CHECK(root != NULL);
        if (root != NULL) {
            ncl_node_map_init(&paths);
            NCL_CHECK_EQ_INT(ncl_root_node_path_map(root, &paths), NCL_OK);
            for (i = 0; i < decl.point_count; i++) {
                char absolute[NCL_PATH_MAX_BUF];
                char id[32];
                ncl_node *node;

                if (ncl_tool_point_is_method(&decl.points[i])) {
                    continue; /* a method: it is not a data item */
                }
                (void)ncl_tool_model_path(&decl, decl.points[i].path, absolute,
                                          sizeof(absolute));
                snprintf(id, sizeof(id), "p%u", (unsigned)i);
                node = ncl_node_find_by_id(root, id);
                NCL_CHECK(node != NULL);
                if (node != NULL) {
                    /* 模型里的路径是绝对路径：声明的相对路径 + 设备段 */
                    NCL_CHECK_EQ_STR(ncl_node_path(node), absolute);
                }
                /* 反向也要能查到：拿模型路径去模型里找节点。 */
                NCL_CHECK(ncl_node_map_get(&paths, absolute) != NULL);
            }
            ncl_node_map_free(&paths);
            ncl_node_free(root);
        }
    }

    NCL_TEST_CASE("设备类型在声明里定义一次：配置里写了就必须一致");
    device = ncl_json_new_object();
    (void)ncl_json_obj_set_string(device, "type", "ROBOT");
    ncl_strbuf_reset(&err);
    model = ncl_tool_model(&decl, device, &err);
    NCL_CHECK(model == NULL);
    NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "不一致") != NULL);
    ncl_json_free(device);
    device = ncl_json_new_object();
    (void)ncl_json_obj_set_string(device, "type", "MACHINE");
    ncl_strbuf_reset(&err);
    model = ncl_tool_model(&decl, device, &err); /* 同名的就在 */
    NCL_CHECK(model != NULL);
    ncl_json_free(model);
    ncl_json_free(device);
    ncl_strbuf_free(&err);
}

/*
 * 声明路径有多深，模型树就有多深：除了最后一段（数据对象），前面每一段都是一个
 * 组件；子组件也能用 "type@number" 区分（这里是两个 SUB，number 1 和 2）。
 */
static void test_nested_components(void)
{
    ncl_tool_decl decl = fixture_decl();
    ncl_tool_point points[2];
    ncl_json *model;
    ncl_strbuf err;

    NCL_TEST_CASE("路径任意深：中间每段是组件，子组件的 number 也照写");
    points[0] = decl.points[1]; /* 一个 config（/NAME）换条深路径 */
    points[0].path = "/CONTROLLER/SUB@1/PARAM";
    points[1] = decl.points[1];
    points[1].path = "/CONTROLLER/SUB@2/PARAM";
    decl.points = points;
    decl.point_count = 2;
    ncl_strbuf_init(&err);
    model = ncl_tool_model(&decl, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model != NULL) {
        ncl_json *device =
            ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
        ncl_json *components =
            device != NULL ? ncl_json_obj_get(device, "components") : NULL;
        ncl_json *controller;
        ncl_json *subs;

        NCL_CHECK(components != NULL);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(components), 1);
        controller = ncl_json_arr_get(components, 0);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(controller, "type"),
                         "CONTROLLER");
        /* 两层组件挂在 CONTROLLER 底下，各自带 number */
        subs = ncl_json_obj_get(controller, "components");
        NCL_CHECK_EQ_INT(ncl_json_arr_len(subs), 2);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(subs, 0),
                                                 "type"),
                         "SUB");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(subs, 0),
                                                 "number"),
                         "1");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(subs, 1),
                                                 "number"),
                         "2");
        NCL_CHECK_EQ_INT(
            ncl_json_arr_len(
                ncl_json_obj_get(ncl_json_arr_get(subs, 0), "configs")),
            1);

        /* 树的路径 = 设备段 + 整条声明路径（最后一段是数据对象） */
        {
            char *text = ncl_json_write_string(model);
            ncl_node *root = text != NULL ? ncl_root_node_parse(text) : NULL;
            ncl_node *node = root != NULL ? ncl_node_find_by_id(root, "p1") : NULL;

            NCL_CHECK(node != NULL);
            if (node != NULL) {
                NCL_CHECK_EQ_STR(ncl_node_path(node),
                                 "/MACHINE/CONTROLLER/SUB@2/PARAM");
            }
            ncl_node_free(root);
            ncl_free_safe(text);
        }
        ncl_json_free(model);
    }
    ncl_strbuf_free(&err);
}

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

static ncl_message *query_op(const char *path, const char *operation)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", operation);
    (void)ncl_message_set_message_id(request, "q2");
    (void)ncl_message_add_query_request_item(request, item);
    return request;
}

static ncl_message *set_op(const char *path, const char *operation,
                           long long value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", operation);
    (void)ncl_params_set_int(&item->params, "value", value);
    (void)ncl_message_set_message_id(request, "s2");
    (void)ncl_message_add_set_request_item(request, item);
    return request;
}

/* ---------------------------------------------------------- 集合类数据对象 -- */

/*
 * list / dict 类型的数据对象：标准第 5 部分给它们单独定义了 get_length、
 * get_keys、get_attributes（表 11）与 add、delete（表 13）。声明把要的操作按位
 * 写全（NCL_DATAITEM_OPS / NCL_CONFIG_OPS），宿主按位绑 —— 文件（dict）就是这样。
 */

static ncl_operation g_collection_op;
static int g_collection_attributes;
static int g_collection_adds;

static ncl_err collection_dispatch(void *ctx, const ncl_tool_point *self,
                                   ncl_operation op, const ncl_json *params,
                                   ncl_json **result, char **reason)
{
    (void)ctx;
    (void)params;
    g_collection_op = op;
    switch (op) {
    case NCL_OP_GET_VALUE:
        return ncl_tool_reply_text(result, "{}");
    case NCL_OP_GET_ATTRIBUTES:
        g_collection_attributes++;
        return ncl_tool_reply_text(result, "[]");
    case NCL_OP_SET_VALUE:
    case NCL_OP_DELETE:
        return ncl_tool_reply_bool(result, true);
    case NCL_OP_ADD:
        g_collection_adds++;
        return ncl_tool_reply_bool(result, true);
    default:
        break;
    }
    return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                         "unsupported operation on %s", self->path);
}

/** dict 的操作集 = 读、写、取属性、新建、删除；刀具列表（list）只要读那几种。 */
static const ncl_tool_point k_collection_points[] = {
    NCL_CONFIG_OPS("/CONTROLLER/FILE", collection_dispatch, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES) |
                       NCL_OP_BIT(NCL_OP_ADD) | NCL_OP_BIT(NCL_OP_DELETE))
    NCL_CONFIG_OPS("/CONTROLLER/TOOL", collection_dispatch, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_KEYS))
    /* COORDINATE 与刀具表一样是一张表（LIST）。 */
    NCL_CONFIG_OPS("/CONTROLLER/COORDINATE", collection_dispatch, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_KEYS))
};

static ncl_tool_decl file_decl(void)
{
    ncl_tool_decl decl = fixture_decl();

    decl.name = "file";
    decl.sample_ms = 0; /* 文件不是采样源 */
    decl.points = k_collection_points;
    decl.point_count =
        sizeof(k_collection_points) / sizeof(k_collection_points[0]);
    return decl;
}

/* -------------------------------------------------------- 还没实现的帧 -- */

/*
 * 协议调用还没实现的点位：声明得和别的点位一模一样（有函数、有操作），差别只在
 * 函数回什么 —— NCL_ERR_UNAVAILABLE（见 ncl_common.h）："这一份 client 还没有它
 * 要的协议调用"。模型里有它、客户端问它有明确答复、采样通道里占着位置、轮询不碰
 * 它、§6 审计里也没有它 —— 全是这一个码说了算，点位表上没有任何"待抓包"的形状。
 *
 * 自己的声明表，夹具那张表不动，别的用例不受影响。
 */

/** 装成 client 的语义函数：帧还没抓到，只报"还没有"，不编值。 */
static ncl_err unreadable_f64(void *instance, long long arg, double *value)
{
    (void)instance;
    (void)arg;
    (void)value;
    return NCL_ERR_UNAVAILABLE;
}

/** 同上，结构化出参那一支（报警是 {"number","text"}）。 */
static ncl_err unreadable_json(void *instance, ncl_json **value)
{
    (void)instance;
    if (value != NULL) {
        *value = NULL;
    }
    return NCL_ERR_UNAVAILABLE;
}

/** 自己带理由的那一种：照常 ncl_tool_fail()，理由原样进应答（工具层只看码）。 */
static ncl_err unreadable_told(void *ctx, const ncl_tool_point *self,
                               ncl_operation op, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    (void)ctx;
    (void)self;
    (void)op;
    (void)params;
    (void)result;
    return ncl_tool_fail(reason, NCL_ERR_UNAVAILABLE, "报警：帧待抓包（%s）",
                         "cnc_rdalmmsg2");
}

static const ncl_tool_point k_unreadable_points[] = {
    NCL_DATAITEM_SAMPLED("/STATUS", fixture_dispatch, &k_run_item)
    NCL_DATAITEM_JSON_SAMPLED("/WARNING", unreadable_json)
    NCL_DATAITEM_F64("/AXIS@X/POSITION@CMD", unreadable_f64, 0)
    NCL_DATAITEM_F64("/AXIS@Y/POSITION@CMD", unreadable_f64, 0)
    NCL_DATAITEM("/ALARM", unreadable_told, NULL)
};

static ncl_tool_decl unreadable_decl(void)
{
    ncl_tool_decl decl = fixture_decl();

    decl.points = k_unreadable_points;
    decl.point_count =
        sizeof(k_unreadable_points) / sizeof(k_unreadable_points[0]);
    return decl;
}

static void test_unreadable(void)
{
    ncl_tool_decl decl = unreadable_decl();
    ncl_strbuf err;
    ncl_json *model;
    char *model_json;
    ncl_server_options options;
    ncl_server *server;
    ncl_tool_registration *registration = NULL;

    NCL_TEST_CASE("a point whose protocol call is not implemented is a point "
                  "like any other");
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK(ncl_tool_point_handles(&decl.points[1], NCL_OP_GET_VALUE));
    NCL_CHECK(decl.points[1].sampled);
    /* 有函数 —— "还没有"这件事就写在 client 的那个函数里，不在点位表上。 */
    NCL_CHECK(decl.points[1].fn != NULL);
    {
        char name[256];

        NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[1], name, sizeof(name)),
                         "WARNING");
        /* 路径尾段重名的两个目标位置靠整条路径分开，校验不会把它们当成撞名。*/
        NCL_CHECK_EQ_STR(ncl_tool_point_name(&decl.points[2], name, sizeof(name)),
                         "AXIS_X.POSITION_CMD");
    }

    NCL_TEST_CASE("a point with no function is refused, whatever it answers");
    {
        ncl_tool_point points[2];
        ncl_tool_decl broken = decl;

        points[0] = decl.points[0];
        points[1] = decl.points[1];
        points[1].fn = NULL;
        broken.points = points;
        broken.point_count = 2;
        expect_refused(&broken, "has no function");
    }

    NCL_TEST_CASE("the model carries them, and the channel keeps their place");
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

        NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 3); /* STATUS, WARNING, ALARM */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(warning, "name"),
                         "报警信息");
        /* 默认采样通道：抽样的那个占着位置（现场要求报警进通道），
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
        if (registration != NULL) {
            /* 学之前谁也不知道：读一次，答"还读不了"，理由说清是哪条路径。 */
            NCL_CHECK(!ncl_tool_point_unavailable(registration, 1));
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
                              strstr(item->reason, "还读不了") != NULL);
                    NCL_CHECK(item->reason != NULL &&
                              strstr(item->reason, "/MACHINE/WARNING") != NULL);
                }
                ncl_message_free(response);
            }
            /* 学过就记住了：宿主据此不再轮询它（ncl_host_point_unavailable）。 */
            NCL_CHECK(ncl_tool_point_unavailable(registration, 1));
            NCL_CHECK(!ncl_tool_point_unavailable(registration, 0));

            /* 作者自己带了理由的那种：原样出去，工具层不重写。 */
            request = query("/MACHINE/ALARM");
            response = ncl_server_invoke_query(server, request);
            ncl_message_free(request);
            NCL_CHECK(response != NULL);
            if (response != NULL) {
                item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
                NCL_CHECK(item != NULL);
                if (item != NULL) {
                    NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_NG);
                    NCL_CHECK(item->reason != NULL &&
                              strstr(item->reason, "cnc_rdalmmsg2") != NULL);
                }
                ncl_message_free(response);
            }
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

static void test_collection_ops(void)
{
    ncl_tool_decl decl = file_decl();
    ncl_tool_decl broken;
    ncl_tool_point point;
    ncl_strbuf err;
    ncl_json *model;
    char *model_json;
    ncl_server_options options;
    ncl_server *server;
    ncl_tool_registration *registration = NULL;

    ncl_strbuf_init(&err);

    NCL_TEST_CASE("操作集按位写全：dict 的读、写、取属性、新建、删除");
    NCL_CHECK_EQ_INT(ncl_tool_validate(&decl, &err), NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK(!ncl_tool_point_is_method(&decl.points[0]));
    NCL_CHECK(ncl_tool_point_handles(&decl.points[0], NCL_OP_GET_ATTRIBUTES));
    NCL_CHECK(ncl_tool_point_handles(&decl.points[0], NCL_OP_ADD));
    NCL_CHECK(!ncl_tool_point_handles(&decl.points[0], NCL_OP_GET_KEYS));

    NCL_TEST_CASE("只声明一个写操作（add）也要可读：可写必然可读");
    broken = decl;
    point = k_collection_points[0];
    point.ops = NCL_OP_BIT(NCL_OP_ADD);
    broken.points = &point;
    broken.point_count = 1;
    expect_refused(&broken, "can be written but not read");

    NCL_TEST_CASE("声明里写不出来的操作名不许出现在点位名里");
    point = k_collection_points[0];
    point.path = "/CONTROLLER/FILE.add";
    broken.points = &point;
    broken.point_count = 1;
    expect_refused(&broken, ".add");

    model = ncl_tool_model(&decl, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_strbuf_free(&err);
        return;
    }

    NCL_TEST_CASE("模型里 dataType 跟着字典走：FILE / 参数是 HASH，表类的是 LIST");
    {
        ncl_json *device =
            ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
        ncl_json *controller =
            ncl_json_arr_get(ncl_json_obj_get(device, "components"), 0);
        ncl_json *configs = ncl_json_obj_get(controller, "configs");
        ncl_json *file_item = ncl_json_arr_get(configs, 0);
        ncl_json *tool_item = ncl_json_arr_get(configs, 1);
        ncl_json *coordinate_item = ncl_json_arr_get(configs, 2);

        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(controller, "type"),
                         "CONTROLLER");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(file_item, "type"), "FILE");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(file_item, "dataType"), "HASH");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(tool_item, "type"), "TOOL");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(tool_item, "dataType"), "LIST");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(coordinate_item, "type"),
                         "COORDINATE");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(coordinate_item, "dataType"),
                         "LIST");
    }

    model_json = ncl_json_write_string(model);
    ncl_json_free(model);
    NCL_CHECK(model_json != NULL);
    if (model_json == NULL) {
        ncl_strbuf_free(&err);
        return;
    }

    memset(&options, 0, sizeof(options));
    options.sn = "V000000002";
    options.model_json = model_json;
    server = ncl_server_create(&options);
    ncl_free_safe(model_json);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        ncl_strbuf_free(&err);
        return;
    }

    NCL_TEST_CASE("宿主按位绑定：11 个操作 = 11 个方法 + 11 条绑定");
    NCL_CHECK_EQ_INT(ncl_tool_register(server, &decl, NULL, &k_sink,
                                       &registration, &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK_EQ_INT(ncl_server_binding_count(server), 22);
    NCL_CHECK_EQ_INT(ncl_server_operation_count(server), 11);

    NCL_TEST_CASE("get_attributes 原样送到点位的函数");
    {
        ncl_message *request =
            query_op("/MACHINE/CONTROLLER/FILE", "get_attributes");
        ncl_message *response = ncl_server_invoke_query(server, request);
        ncl_query_response_item *item;

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            item = (ncl_query_response_item *)ncl_message_item_at(response, 0);
            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
        NCL_CHECK_EQ_INT(g_collection_attributes, 1);
        NCL_CHECK_EQ_INT((int)g_collection_op, (int)NCL_OP_GET_ATTRIBUTES);
    }

    NCL_TEST_CASE("add 也算写：点位拿到 add，审计拿到被覆盖的值");
    {
        int writes_before = g_sink_writes;
        ncl_message *request = set_op("/MACHINE/CONTROLLER/FILE", "add", 7);
        ncl_message *response = ncl_server_invoke_set(server, request);

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_set_response_item *item =
                (ncl_set_response_item *)ncl_message_item_at(response, 0);

            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
        NCL_CHECK_EQ_INT(g_collection_adds, 1);
        NCL_CHECK_EQ_INT((int)g_collection_op, (int)NCL_OP_ADD);
        NCL_CHECK_EQ_INT(g_sink_writes, writes_before + 1);
        NCL_CHECK_EQ_INT(g_sink_new, 7);
        NCL_CHECK_EQ_STR(g_sink_path, "/MACHINE/CONTROLLER/FILE");
    }

    NCL_TEST_CASE("没声明的操作走不通：宿主只绑声明过的");
    {
        ncl_message *request =
            query_op("/MACHINE/CONTROLLER/FILE", "get_keys");
        ncl_message *response = ncl_server_invoke_query(server, request);

        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *item =
                (ncl_query_response_item *)ncl_message_item_at(response, 0);

            NCL_CHECK(item != NULL);
            if (item != NULL) {
                NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_NG);
            }
            ncl_message_free(response);
        }
        ncl_message_free(request);
    }

    ncl_tool_unregister(&decl, registration);
    ncl_server_free(server);
    ncl_strbuf_free(&err);
}

/** The component of @p components that carries @p type, or NULL. */
static ncl_json *find_component(ncl_json *components, const char *type)
{
    size_t i;

    for (i = 0; i < ncl_json_arr_len(components); i++) {
        ncl_json *component = ncl_json_arr_get(components, i);
        const char *other = ncl_json_obj_get_string(component, "type");

        if (other != NULL && strcmp(other, type) == 0) {
            return component;
        }
    }
    return NULL;
}

/**
 * 一个宿主发布多个工具（适配器自己的 + file 工具）：模型是一份 —— 两台工具的
 * 点位挂在同一台设备下，共用的组件只出现一次，id 重排过所以不撞。
 */
static void test_several_tools(void)
{
    ncl_tool_decl first = fixture_decl();
    ncl_tool_decl second = file_decl();
    ncl_strbuf err;
    ncl_json *model;
    ncl_json *device;
    ncl_json *components;
    ncl_json *controller;
    ncl_json *configs;

    ncl_strbuf_init(&err);

    NCL_TEST_CASE("两份声明合成一份模型：共用的组件只有一份，id 接着排");
    model = ncl_tool_model(&first, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_strbuf_free(&err);
        return;
    }
    NCL_CHECK(ncl_tool_model_add(model, &second, NULL, &err) == model);

    device = ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(model, "devices")), 1);
    /* 夹具的 dataItems（STATUS@RUN、MODE）与 configs（NAME + 采样通道）还在。 */
    NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(device, "dataItems")), 2);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(device, "configs")), 2);
    /* CONTROLLER 是两台工具共用的组件：只有一份。 */
    components = ncl_json_obj_get(device, "components");
    NCL_CHECK_EQ_INT(ncl_json_arr_len(components), 1);
    controller = find_component(components, "CONTROLLER");
    NCL_CHECK(controller != NULL);
    if (controller != NULL) {
        configs = ncl_json_obj_get(controller, "configs");
        NCL_CHECK_EQ_INT(ncl_json_arr_len(configs), 3);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(configs, 0),
                                                 "type"),
                         "FILE");
        /* id 是模型自己的编号：第一台工具的点位排到 p2（它的方法不进模型），
         * 第二台工具从 p3 接着排。路径才是身份，宿主按路径查节点。 */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(configs, 0),
                                                 "id"),
                         "p3");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(configs, 2),
                                                 "id"),
                         "p5");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(configs, 2),
                                                 "type"),
                         "COORDINATE");
    }

    NCL_TEST_CASE("不是同一台设备的声明合不进来");
    {
        ncl_tool_decl foreign = second;
        ncl_json *other_device = ncl_json_new_object();
        ncl_strbuf probe;

        /* 设备段只在 device.type 里写一次，所以"另一台设备"就是这个对象不一样 */
        NCL_CHECK(other_device != NULL);
        (void)ncl_json_obj_set_string(other_device, "type", "PLC1");
        ncl_strbuf_init(&probe);
        NCL_CHECK(ncl_tool_model_add(model, &foreign, other_device, &probe) ==
                  NULL);
        NCL_CHECK(probe.len > 0);
        ncl_strbuf_free(&probe);
        ncl_json_free(other_device);
    }

    ncl_json_free(model);
    ncl_strbuf_free(&err);
}

NCL_TEST_MAIN_BEGIN()
    test_declaration();
    test_validate();
    test_model();
    test_model_paths_match_declaration();
    test_nested_components();
    test_helpers();
    test_register_and_invoke();
    test_unreadable();
    test_collection_ops();
    test_several_tools();
NCL_TEST_MAIN_END()
