/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 新代 SYNTEC 适配器 —— 一个文件一台机床，与 plugins/focas.c 同构。
 *
 * 这份文件只做两件事：
 *
 *   1. **绑定**：把 client（clients/syntec）的语义函数挂到模型路径上，一行一个点。
 *      "STATUS 是状态索引 4 的枚举"、"PART_COUNT 是寄存器 1000"、"FEED_SPEED 要问
 *      三帧"这些知识都在 client 里（10 册 §3.1/§3.2），这里不重复。
 *   2. **覆盖**：字典类型与协议取值不一致的一处自己写一小段 —— LINE_NUMBER 按表 7
 *      是 string，client 读回来是数，这里落成文本。
 *
 * **路径按 iNC-BOX 的模型定义**（现场盒子的 `lua_mod/syntec_mod.lua` 9 项 +
 * 它的标准路径字典）：`/STATUS`、`/PART_COUNT`、`/FEED_SPEED`、`/FEED_OVERRIDE`、
 * `/SPINDLE_OVERRIDE`、**`/SPINDLE_SPEED`**、`/CONTROLLER/PROGRAM`、
 * **`/CONTROLLER/LINE_NUMBER`**、**`/CONTROLLER/WARNING`** —— 这样现场那套客户端脚本
 * 不用改路径。仓库里 KND 也是这套命名；FANUC 那条（`/LINE_NUMBER`、`/WARNING`、
 * `/MOTOR@S1/SPEED`）是历史差异，见 10 册 §11.2。
 *
 * 帧、命令号、九项的请求/应答布局一个字节都不在这份文件里 —— 那是 client 的事。
 *
 * **九项是现场闭环过的**（10 册 §3.1/§3.2：请求形状抓到了、应答读法逐项试出来了）：
 * STATUS / PART_COUNT / LINE_NUMBER / PROGRAM / FEED_SPEED / FEED_OVERRIDE /
 * SPDL_SPEED / SPDL_OVERRIDE / WARNING。默认采样通道按现场口径放四样：设备状态、
 * 加工计件、程序名称、报警；其余按需读。
 *
 * 只读：现场网关那一侧的新代也只有读（`/SYNTEC/CNC/*` 12 条 = Open/Close/GetResponse
 * + 九项）。写入类（宏、参数、刀补、PLC 写、程序上下行）client 里没有对应调用，
 * 这里就不声明 —— 没有声明的操作走不到，也不会假装能写。
 */
#include "nclink/ncl_tool.h"

#include <stdio.h>
#include <string.h>

#include "nclink/clients/syntec.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_json.h"

/* ---------------------------------------------------------------- 连接 ---- */

/**
 * 配置里的 parameters 交给 client。会话是懒的：机床没开机不影响设备程序启动，
 * 谁问值，谁拿到一条明确的"读不到"。
 */
static void *syntec_open(const ncl_json *params, char **err)
{
    ncl_syntec_config config;

    ncl_syntec_config_default(&config);
    config.host = ncl_tool_param_str(params, "host", "");
    config.port = (unsigned)ncl_tool_param_int(params, "port", 8000);
    config.connect_timeout_ms = (unsigned)ncl_tool_param_int(
        params, "connectTimeoutMs", (long long)config.connect_timeout_ms);
    config.timeout_ms = (unsigned)ncl_tool_param_int(
        params, "timeoutMs", (long long)config.timeout_ms);
    config.retries = (unsigned)ncl_tool_param_int(params, "retries", 0);
    return ncl_syntec_open(&config, err);
}

static void syntec_close(void *ctx)
{
    ncl_syntec_close((ncl_syntec *)ctx);
}

/* -------------------------------------------------------------- 覆盖档 ---- */

/** 表 7 的 LINE_NUMBER 是 string：client 读回来是数，这里落成文本。 */
static ncl_err syntec_line_number(void *ctx, char *out, size_t cap)
{
    long long value = 0;
    ncl_err rc = ncl_syntec_line_number((ncl_syntec *)ctx, &value);

    if (rc != NCL_OK) {
        return rc;
    }
    snprintf(out, cap, "%lld", value);
    return NCL_OK;
}

/** 主轴转速（rpm）落成数：iNC-BOX 的名字是 `/SPINDLE_SPEED`。 */
static ncl_err syntec_spindle_speed(void *ctx, double *out)
{
    long long value = 0;
    ncl_err rc = ncl_syntec_spindle_speed((ncl_syntec *)ctx, &value);

    if (rc != NCL_OK) {
        return rc;
    }
    *out = (double)value;
    return NCL_OK;
}

/* ---------------------------------------------------------------- 位置 ---- */

/*
 * 位置是**状态区**（10 册 §11.3.1）：机械 101 / 相对 141 / 绝对 181 / 剩余 221。
 * 值 = 每轴一个 int16 除以 10^小数位（小数位本身是区 261，21A 车床答 3，与画面
 * `0.000` 一致）。路径按 iNC-BOX 的格子：**机械坐标就是实际位置**，落
 * `/AXIS@<轴>/MOTOR/POSITION`；另外三组落在同一层的 `MOTOR/VARIABLE@*`。
 *
 * 路径是声明里写死的、也不会变（iNC-BOX 的格子），**轴号在读值的时候才现查**：
 * 点位的 `arg` 就是路径里那个字母（`'X'` / `'Z'`），先拿它去控制器问“这个轴名是
 * 第几个槽”，再按那个槽号从状态区取值。客户端的 `get_MachineCoordinate()` 正是
 * 这个口径：`r[i] = data[EnableAxisMappingID[i]]`——`data` 按槽排。
 *
 * 名字对不上（控制器没启用这个轴、或这名字根本不在表里）就照实报
 * NCL_ERR_NOT_FOUND，不拿声明顺序去猜轴号——猜错就是读到另一个轴的位置。
 */
static ncl_err syntec_axis_zone(void *ctx, long long arg, unsigned zone,
                                double *out)
{
    double values[NCL_SYNTEC_POSITION_MAX_AXES];
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    char name[2];
    unsigned slot = 0;
    size_t count = 0;
    ncl_err rc;

    if (arg <= 0 || arg > 0x7F) {
        return NCL_ERR_INVALID_ARG;
    }
    name[0] = (char)arg;
    name[1] = '\0';
    rc = ncl_syntec_axis_index(syntec, name, &slot, &count);
    if (rc != NCL_OK) {
        return rc;
    }
    if (count > NCL_SYNTEC_POSITION_MAX_AXES) {
        count = NCL_SYNTEC_POSITION_MAX_AXES; /* 本实现的上限（16 个轴槽） */
    }
    if (slot >= count) {
        return NCL_ERR_RANGE; /* 槽号比读得到的还靠后：控制器说得不对 */
    }
    rc = ncl_syntec_position(syntec, zone, count, values);
    if (rc != NCL_OK) {
        return rc;
    }
    *out = values[slot];
    return NCL_OK;
}

/** 机械坐标 = **实际位置**（区 101）。 */
static ncl_err syntec_machine_position(void *ctx, long long arg, double *out)
{
    return syntec_axis_zone(ctx, arg, NCL_SYNTEC_ZONE_MACHINE, out);
}

/** 绝对坐标（区 181）。 */
static ncl_err syntec_absolute_position(void *ctx, long long arg, double *out)
{
    return syntec_axis_zone(ctx, arg, NCL_SYNTEC_ZONE_ABSOLUTE, out);
}

/** 相对坐标（区 141）。 */
static ncl_err syntec_relative_position(void *ctx, long long arg, double *out)
{
    return syntec_axis_zone(ctx, arg, NCL_SYNTEC_ZONE_RELATIVE, out);
}

/** 剩余距离（区 221）。 */
static ncl_err syntec_distance_position(void *ctx, long long arg, double *out)
{
    return syntec_axis_zone(ctx, arg, NCL_SYNTEC_ZONE_DISTANCE, out);
}

/**
 * 指令位置（`SERVO_DRIVER/POSITION`，§11.3.4）：控制器里还没有这一项，照实报"读不了"。
 * 名字先占位（模型一次配全），实现等抓到来源再补——不会拿"实际 + 剩余距离"去凑。
 */
static ncl_err syntec_command_position(void *ctx, long long arg, double *out)
{
    (void)arg;
    return ncl_syntec_command_position((ncl_syntec *)ctx, out);
}

/* ------------------------------------------------------------ 现场调试 ---- */

/**
 * `/SESSION`：会话现在什么样 + 上一次失败的原话。不进模型、不参与采样，只在
 * 现场问"为什么读不到"时用（client 里那句"单位换算表待抓包""报警条目布局待抓包"
 * 就是从这里看到的）。
 */
static ncl_err syntec_session(void *ctx, const ncl_json *params, ncl_json **result,
                              char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    const char *last = ncl_syntec_last_error(syntec);
    ncl_json *reply = ncl_json_new_object();

    (void)params;
    if (reply == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
    }
    ncl_json_obj_set_bool(reply, "open", ncl_syntec_is_open(syntec));
    ncl_json_obj_set_string(reply, "lastError", last != NULL ? last : "");
    *result = reply;
    return NCL_OK;
}

/**
 * `/AXES`：控制器自己说有哪些轴（§11.4）。
 *
 * 参数表的口径（槽号、端口号、名字）与位置区的口径（`stateCount`：状态区一项
 * 里有几个轴值，等于客户端的 `get_MaxUsedAxisID() + 1`）都在这里：位置数组第 i
 * 个就是 `axes[i]`。轴表读不到就照实回 NCL_ERR_UNAVAILABLE，不猜一个名字出来。
 */
static ncl_err syntec_axes(void *ctx, const ncl_json *params, ncl_json **result,
                           char **reason)
{
    ncl_syntec_axis axes[NCL_SYNTEC_AXIS_SLOTS];
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    ncl_json *array;
    ncl_json *reply;
    size_t count = 0;
    size_t i;
    ncl_err rc;

    (void)params;
    rc = ncl_syntec_axes(syntec, axes, sizeof(axes) / sizeof(axes[0]), &count);
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
    }
    array = ncl_json_new_array();
    reply = ncl_json_new_object();
    if (array == NULL || reply == NULL) {
        ncl_json_free(array);
        ncl_json_free(reply);
        return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
    }
    for (i = 0; i < count; i++) {
        ncl_json *entry = ncl_json_new_object();

        if (entry == NULL) {
            ncl_json_free(array);
            ncl_json_free(reply);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        /* index：位置数组下标；axis：参数表里的第几个轴（1 起数）。 */
        (void)ncl_json_obj_set_int(entry, "index", (long long)i);
        (void)ncl_json_obj_set_int(entry, "axis", (long long)axes[i].slot + 1);
        (void)ncl_json_obj_set_int(entry, "port", (long long)axes[i].port);
        (void)ncl_json_obj_set_string(entry, "name", axes[i].name);
        if (ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(array);
            ncl_json_free(reply);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
    }
    (void)ncl_json_obj_set_int(reply, "count", (long long)count);
    /* 状态区一项的条目数：客户端用 get_MaxUsedAxisID() + 1 当读数长度。 */
    (void)ncl_json_obj_set_int(reply, "stateCount",
                               (long long)axes[count - 1].slot + 1);
    (void)ncl_json_obj_set(reply, "axes", array);
    *result = reply;
    return NCL_OK;
}

/** §6 的审计要原始报文：问 client 一句就够，账由宿主管。 */
/* ---------------------------------------------------------------- 参数 ---- */

/** 一次读多少条参数：每条一次往返，别让人一口气点四千次。 */
#define SYNTEC_PARAM_BATCH_MAX 64u
/** 翻页取元数据时一页多少条。 */
#define SYNTEC_PARAM_PAGE 16u

/**
 * params.keys：按标准是个数组，也认单个（"321" / 321 都行）。
 * @p index 超出时回 false。
 */
static bool syntec_param_key(const ncl_json *keys, size_t index, long long *out)
{
    const ncl_json *item = keys;
    size_t count;

    if (keys == NULL) {
        return false;
    }
    count = ncl_json_arr_len(keys);
    if (count > 0) {
        if (index >= count) {
            return false;
        }
        item = ncl_json_arr_get(keys, index);
    } else if (index > 0) {
        return false;
    }
    return item != NULL && ncl_json_as_int(item, out);
}

/** params.keys 里有几个号（单个算一个，没有算零）。 */
static size_t syntec_param_key_count(const ncl_json *keys)
{
    size_t count;

    if (keys == NULL) {
        return 0;
    }
    count = ncl_json_arr_len(keys);
    return count > 0 ? count : 1u;
}

/**
 * `/CONTROLLER/PARAMETER`：系统参数（§11.4）。
 *
 * 按设备模型的摆法（`examples/device_model.c` 的 CONTROLLER 组件下
 * `type: PARAMETER`、dict；册 4 说它归 configs、`dataType` = `HASH`），参数是
 * **配置对象**而不是数据项——所以这里是 `NCL_CONFIG_OPS`，按标准的 Query 操作回答：
 *
 *   get_length      参数表有多少条
 *   get_keys        参数号清单（按表里的顺序）
 *   get_value       按号读值：`keys` 给号，答 `{"321":100,...}`
 *   get_attributes  按号取元数据：答 `[{"no","title","flags","fallback"}, ...]`
 *
 * 号不在表里回 `NCL_ERR_NOT_FOUND`；一次最多 `SYNTEC_PARAM_BATCH_MAX` 条。
 * **写（set_value / add / delete）不声明**：控制器侧没验过怎么写参数，
 * 没声明的操作由宿主回 "Unsupported Operation"，比给个假写入口诚实。
 */
static ncl_err syntec_parameter(void *ctx, const ncl_tool_point *self,
                                ncl_operation op, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    ncl_syntec_param_spec page[SYNTEC_PARAM_PAGE];
    size_t total = 0;
    size_t got = 0;
    size_t i;
    ncl_err rc;

    (void)self;
    switch (op) {
    case NCL_OP_GET_LENGTH: /* 参数表有多少条 */
        rc = ncl_syntec_param_table(syntec, 0, 1, page, &got, &total);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        return ncl_tool_reply_int(result, (long long)total);

    case NCL_OP_GET_KEYS: /* 参数号清单，按表里的顺序 */
        rc = ncl_syntec_param_table(syntec, 0, 1, page, &got, &total);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        {
            ncl_json *array = ncl_json_new_array();

            if (array == NULL) {
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            for (i = 0; i < total; i += SYNTEC_PARAM_PAGE) {
                size_t want = total - i < SYNTEC_PARAM_PAGE ? total - i
                                                            : SYNTEC_PARAM_PAGE;
                size_t k;

                rc = ncl_syntec_param_table(syntec, i, want, page, &got, &total);
                if (rc != NCL_OK) {
                    ncl_json_free(array);
                    return ncl_tool_fail(reason, rc, "%s",
                                         ncl_syntec_last_error(syntec));
                }
                for (k = 0; k < got; k++) {
                    char text[16];
                    ncl_json *item;

                    snprintf(text, sizeof(text), "%d", (int)page[k].no);
                    item = ncl_json_new_string(text);
                    if (item == NULL || ncl_json_arr_push(array, item) != NCL_OK) {
                        ncl_json_free(item);
                        ncl_json_free(array);
                        return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                    }
                }
            }
            *result = array;
        }
        return NCL_OK;

    case NCL_OP_GET_VALUE: /* 按号读值 */
    case NCL_OP_GET_ATTRIBUTES: { /* 按号取元数据 */
        size_t count = syntec_param_key_count(keys);
        ncl_json *out;

        if (count == 0) {
            /*
             * 没给 keys：不报错，答一个空的（字典给 {}、表格给 []）。
             * 自检与轮询会对每个点位盲读一次，四千个参数没有"盲读"这一说——
             * 报错只会让现场每次自检都看见一条假的失败。要值就请给 keys。
             */
            *result = op == NCL_OP_GET_VALUE ? ncl_json_new_object()
                                             : ncl_json_new_array();
            return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
        }
        if (count > SYNTEC_PARAM_BATCH_MAX) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)SYNTEC_PARAM_BATCH_MAX);
        }
        out = op == NCL_OP_GET_VALUE ? ncl_json_new_object()
                                     : ncl_json_new_array();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < count; i++) {
            long long no = 0;
            ncl_json *entry;

            if (!syntec_param_key(keys, i, &no) || no < 1 || no > 0xFFFF) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 里第 %u 个不是参数号",
                                     (unsigned)(i + 1));
            }
            if (op == NCL_OP_GET_VALUE) {
                int32_t value = 0;
                char name[16];

                rc = ncl_syntec_param(syntec, (unsigned)no, &value);
                if (rc != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, rc, "%s",
                                         ncl_syntec_last_error(syntec));
                }
                snprintf(name, sizeof(name), "%d", (int)no);
                if (ncl_json_obj_set_int(out, name, (long long)value) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
                continue;
            }
            {
                size_t index = 0;

                rc = ncl_syntec_param_find(syntec, (unsigned)no, &index);
                if (rc != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, rc, "%s",
                                         ncl_syntec_last_error(syntec));
                }
                rc = ncl_syntec_param_table(syntec, index, 1, page, &got, &total);
                if (rc != NCL_OK || got != 1) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, rc != NCL_OK ? rc : NCL_ERR_RANGE,
                                         "%s", ncl_syntec_last_error(syntec));
                }
            }
            entry = ncl_json_new_object();
            if (entry == NULL) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            (void)ncl_json_obj_set_int(entry, "no", page[0].no);
            (void)ncl_json_obj_set_string(entry, "title", page[0].title);
            (void)ncl_json_obj_set_int(entry, "flags", page[0].flags);
            (void)ncl_json_obj_set_int(entry, "fallback", page[0].fallback);
            if (ncl_json_arr_push(out, entry) != NCL_OK) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "参数只答 get_length / get_keys / get_value / get_attributes");
    }
}


static void syntec_last_raw(void *ctx, ncl_tool_frames *out)
{
    const uint8_t *request = NULL;
    const uint8_t *reply = NULL;
    size_t request_len = 0;
    size_t reply_len = 0;

    ncl_syntec_last_raw((ncl_syntec *)ctx, &request, &request_len, &reply,
                        &reply_len);
    out->request = request;
    out->request_len = request_len;
    out->reply = reply;
    out->reply_len = reply_len;
}

/* ------------------------------------------------------------------ 工具 -- */

NCL_TOOL_BEGIN("syntec", "SYNTEC RemoteCNC over TCP (8000), read only",
               "MACHINE", 1000, 1000, syntec_open, syntec_close)

    /* 默认采样通道只放四样（现场口径）：设备状态、加工计件、程序名称、报警。 */
    NCL_DATAITEM_STR_SAMPLED("/STATUS", ncl_syntec_status)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", ncl_syntec_part_count)
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_syntec_program)
    NCL_DATAITEM_JSON_SAMPLED("/CONTROLLER/WARNING", ncl_syntec_warning)

    /* 表 7 的 LINE_NUMBER 是 string，所以走覆盖档；倍率与速度按需读（不进采样通道）。 */
    NCL_DATAITEM_STR("/CONTROLLER/LINE_NUMBER", syntec_line_number)
    NCL_DATAITEM_I64("/FEED_OVERRIDE", ncl_syntec_feed_override)
    NCL_DATAITEM_I64("/SPINDLE_OVERRIDE", ncl_syntec_spindle_override)
    NCL_DATAITEM_F64("/FEED_SPEED", ncl_syntec_feed_speed)

    /* 主轴转速：按 iNC-BOX 的字典 `/SPINDLE_SPEED`（rpm），设备级。 */
    NCL_DATAITEM_F64("/SPINDLE_SPEED", syntec_spindle_speed)

    /* 位置：**九个轴字母都占位**（X/Y/Z/A/B/C/U/V/W），每轴六格（§11.3.3）：
     *   SCREW/POSITION          实际位置（机械坐标 = 实际位置，区 101）
     *   SERVO_DRIVER/POSITION   指令位置（对照 examples/device_model.c 的摆放：实际在
     *                           丝杠侧、指令在驱动侧；**控制器里还没找到这一项**，
     *                           照实报 NCL_ERR_UNAVAILABLE）
     *   MOTOR/POSITION          机械坐标（iNC-BOX 格子，与 SCREW/POSITION 同源）
     *   MOTOR/VARIABLE@ABSOLUTE|RELATIVE|DISTANCE  绝对 / 相对 / 剩余（181/141/221）
     *
     * 路径写死、轴号在读值的时候现查（§11.4）：控制器没配这个轴（轴表里没这个名字）
     * 就报 NCL_ERR_NOT_FOUND，不给数、也不去读别的轴。模型一次配全，不再改。 */
    NCL_DATAITEM_F64("/AXIS@X/SCREW/POSITION", syntec_machine_position, 'X')
    NCL_DATAITEM_F64("/AXIS@Y/SCREW/POSITION", syntec_machine_position, 'Y')
    NCL_DATAITEM_F64("/AXIS@Z/SCREW/POSITION", syntec_machine_position, 'Z')
    NCL_DATAITEM_F64("/AXIS@A/SCREW/POSITION", syntec_machine_position, 'A')
    NCL_DATAITEM_F64("/AXIS@B/SCREW/POSITION", syntec_machine_position, 'B')
    NCL_DATAITEM_F64("/AXIS@C/SCREW/POSITION", syntec_machine_position, 'C')
    NCL_DATAITEM_F64("/AXIS@U/SCREW/POSITION", syntec_machine_position, 'U')
    NCL_DATAITEM_F64("/AXIS@V/SCREW/POSITION", syntec_machine_position, 'V')
    NCL_DATAITEM_F64("/AXIS@W/SCREW/POSITION", syntec_machine_position, 'W')
    NCL_DATAITEM_F64("/AXIS@X/SERVO_DRIVER/POSITION", syntec_command_position, 'X')
    NCL_DATAITEM_F64("/AXIS@Y/SERVO_DRIVER/POSITION", syntec_command_position, 'Y')
    NCL_DATAITEM_F64("/AXIS@Z/SERVO_DRIVER/POSITION", syntec_command_position, 'Z')
    NCL_DATAITEM_F64("/AXIS@A/SERVO_DRIVER/POSITION", syntec_command_position, 'A')
    NCL_DATAITEM_F64("/AXIS@B/SERVO_DRIVER/POSITION", syntec_command_position, 'B')
    NCL_DATAITEM_F64("/AXIS@C/SERVO_DRIVER/POSITION", syntec_command_position, 'C')
    NCL_DATAITEM_F64("/AXIS@U/SERVO_DRIVER/POSITION", syntec_command_position, 'U')
    NCL_DATAITEM_F64("/AXIS@V/SERVO_DRIVER/POSITION", syntec_command_position, 'V')
    NCL_DATAITEM_F64("/AXIS@W/SERVO_DRIVER/POSITION", syntec_command_position, 'W')
    NCL_DATAITEM_F64("/AXIS@X/MOTOR/POSITION", syntec_machine_position, 'X')
    NCL_DATAITEM_F64("/AXIS@Y/MOTOR/POSITION", syntec_machine_position, 'Y')
    NCL_DATAITEM_F64("/AXIS@Z/MOTOR/POSITION", syntec_machine_position, 'Z')
    NCL_DATAITEM_F64("/AXIS@A/MOTOR/POSITION", syntec_machine_position, 'A')
    NCL_DATAITEM_F64("/AXIS@B/MOTOR/POSITION", syntec_machine_position, 'B')
    NCL_DATAITEM_F64("/AXIS@C/MOTOR/POSITION", syntec_machine_position, 'C')
    NCL_DATAITEM_F64("/AXIS@U/MOTOR/POSITION", syntec_machine_position, 'U')
    NCL_DATAITEM_F64("/AXIS@V/MOTOR/POSITION", syntec_machine_position, 'V')
    NCL_DATAITEM_F64("/AXIS@W/MOTOR/POSITION", syntec_machine_position, 'W')
    NCL_DATAITEM_F64("/AXIS@X/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'X')
    NCL_DATAITEM_F64("/AXIS@Y/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'Y')
    NCL_DATAITEM_F64("/AXIS@Z/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'Z')
    NCL_DATAITEM_F64("/AXIS@A/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'A')
    NCL_DATAITEM_F64("/AXIS@B/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'B')
    NCL_DATAITEM_F64("/AXIS@C/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'C')
    NCL_DATAITEM_F64("/AXIS@U/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'U')
    NCL_DATAITEM_F64("/AXIS@V/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'V')
    NCL_DATAITEM_F64("/AXIS@W/MOTOR/VARIABLE@ABSOLUTE", syntec_absolute_position, 'W')
    NCL_DATAITEM_F64("/AXIS@X/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'X')
    NCL_DATAITEM_F64("/AXIS@Y/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'Y')
    NCL_DATAITEM_F64("/AXIS@Z/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'Z')
    NCL_DATAITEM_F64("/AXIS@A/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'A')
    NCL_DATAITEM_F64("/AXIS@B/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'B')
    NCL_DATAITEM_F64("/AXIS@C/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'C')
    NCL_DATAITEM_F64("/AXIS@U/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'U')
    NCL_DATAITEM_F64("/AXIS@V/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'V')
    NCL_DATAITEM_F64("/AXIS@W/MOTOR/VARIABLE@RELATIVE", syntec_relative_position, 'W')
    NCL_DATAITEM_F64("/AXIS@X/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'X')
    NCL_DATAITEM_F64("/AXIS@Y/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'Y')
    NCL_DATAITEM_F64("/AXIS@Z/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'Z')
    NCL_DATAITEM_F64("/AXIS@A/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'A')
    NCL_DATAITEM_F64("/AXIS@B/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'B')
    NCL_DATAITEM_F64("/AXIS@C/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'C')
    NCL_DATAITEM_F64("/AXIS@U/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'U')
    NCL_DATAITEM_F64("/AXIS@V/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'V')
    NCL_DATAITEM_F64("/AXIS@W/MOTOR/VARIABLE@DISTANCE", syntec_distance_position, 'W')

    NCL_METHOD_CALL("/SESSION", syntec_session)
    /* 系统参数（§11.4）：按设备模型摆成**配置对象**（/CONTROLLER/PARAMETER，
     * 册 4 说它归 configs、dataType = HASH），按标准的 Query 操作回答：
     * get_length / get_keys / get_value（按号读值）/ get_attributes（按号取标题）。
     * 写不声明：控制器侧没验过写参数。 */
    NCL_CONFIG_OPS("/CONTROLLER/PARAMETER", syntec_parameter, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_KEYS) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

NCL_TOOL_END_WITH_RAW(syntec_last_raw)

NCL_TOOL_MODULE("1.0.0",
                "新代 SYNTEC（RemoteCNC）适配器，九项按 10 册 §3.1/§3.2 的现场闭环实现")
