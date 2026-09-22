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
 * **读**：现场网关那一侧的新代是读（`/SYNTEC/CNC/*` 12 条 = Open/Close/GetResponse
 * + 九项），这也是这里的主体。
 *
 * **文件**：G 代码上下行走**另一套服务**（§11.11，端口 5572），插件把它接到仓库的文件工具上
 * （`/CONTROLLER/FILE` 的 push / pull / remove）—— 下发、取回、删程序三条都可用，参数名 `filePort`。
 *
 * **写**：参数（`/CONTROLLER/PARAMETER` 的 `set_value`，10 册 §11.6）与刀补
 * （`/CONTROLLER/TOOL` 的 `set_value`，§11.7）都开了 —— 用户口径是**权限在适配器外面控**，
 * 这里只提供能力。宏、PLC 写、程序上下行仍旧不声明：client 里没有对应调用，
 * 没有声明的操作走不到，也不会假装能写。
 */
#include "nclink/ncl_tool.h"

#include <stdio.h>
#include <string.h>

#include "nclink/clients/syntec.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_json.h"

/* ---------------------------------------------------------------- 连接 ---- */

/**
 * 配置里的 parameters 交给 client。会话是懒的：机床没开机不影响设备程序启动，
 * 谁问值，谁拿到一条明确的"读不到"。
 */
/* 文件后端（§11.11）：设备那侧的 `/CONTROLLER/FILE` 靠这三个回调进出。 */
static ncl_err syntec_file_push(void *user, const char *name, const char *path,
                               char **reason);
static ncl_err syntec_file_pull(void *user, const char *name, const char *path,
                               char **reason);
static ncl_err syntec_file_remove(void *user, const char *name, char **reason);

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
    /* §11.11：文件服务走另一条连接（默认 5572），可以单独指一个端口。 */
    config.file_port = (unsigned)ncl_tool_param_int(
        params, "filePort", (long long)config.file_port);
    {
        static ncl_file_backend backend;
        ncl_syntec *syntec = ncl_syntec_open(&config, err);

        if (syntec == NULL) {
            return NULL;
        }
        /* 文件工具（设备侧的 `/CONTROLLER/FILE`）接上这三个回调，像 FOCAS 那样。 */
        memset(&backend, 0, sizeof(backend));
        backend.user = syntec;
        backend.push = syntec_file_push;
        backend.pull = syntec_file_pull;
        backend.remove = syntec_file_remove;
        (void)ncl_file_tool_set_backend(&backend);
        return syntec;
    }
}

static void syntec_close(void *ctx)
{
    /* 会话走了就把文件后端撤掉（不然会留一个指向已释放会话的指针）。 */
    (void)ncl_file_tool_set_backend(NULL);
    ncl_syntec_close((ncl_syntec *)ctx);
}

/** 下发：本地 @p path 的文件 -> 控制器上的 @p name（一帧一块的 FileSending）。 */
static ncl_err syntec_file_push(void *user, const char *name, const char *path,
                               char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)user;
    char *bytes = NULL;
    size_t len = 0;
    ncl_err rc = ncl_file_read_all(path, &bytes, &len);

    if (rc != NCL_OK) {
        if (reason != NULL) {
            *reason = ncl_strdup("读本地文件失败");
        }
        return rc;
    }
    rc = ncl_syntec_file_push(syntec, name, (const uint8_t *)bytes, len);
    ncl_free_safe(bytes);
    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_syntec_last_error(syntec));
    }
    return rc;
}

/** 取回：控制器上的 @p name -> 本地 @p path。 */
static ncl_err syntec_file_pull(void *user, const char *name, const char *path,
                               char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)user;
    uint8_t *bytes = NULL;
    size_t len = 0;
    ncl_err rc = ncl_syntec_file_pull(syntec, name, &bytes, &len);

    if (rc != NCL_OK) {
        if (reason != NULL) {
            *reason = ncl_strdup(ncl_syntec_last_error(syntec));
        }
        return rc;
    }
    rc = ncl_file_write_all(path, bytes, len);
    ncl_mem_free(bytes);
    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup("写本地文件失败");
    }
    return rc;
}

static ncl_err syntec_file_remove(void *user, const char *name, char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)user;
    ncl_err rc = ncl_syntec_file_delete(syntec, name);

    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_syntec_last_error(syntec));
    }
    return rc;
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
 *
 * **写开 `set_value`**（§11.6，2026-09-22 在 21A 上闭环）：`{"keys":"321","value":111}`
 * 或直接给字典 `{"321":111}`。`add` / `delete` 不声明：参数表是控制器定的，没有这两个动作。
 * 权限、白名单、二次确认都在适配器外面（用户口径："权限在外面控制"）。
 */
static ncl_err syntec_parameter(void *ctx, const ncl_tool_point *self,
                                ncl_operation op, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    ncl_syntec_param_spec page[SYNTEC_PARAM_PAGE];
    ncl_json *out = NULL; /* 读（get_value/get_attributes）与写（set_value）都用它 */
    size_t total = 0;
    size_t got = 0;
    size_t i;
    ncl_err rc;

    (void)self;
    switch (op) {
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
    case NCL_OP_SET_VALUE: /* 写参数（§11.6）：权限在外面控制，这里只管写 */
    {
        const ncl_json *value = ncl_params_get(params, "value");

        if (value != NULL) { /* {"keys":"321","value":100} */
            long long no = 0;
            long long new_value = 0;

            if (!syntec_param_key(keys, 0, &no) ||
                !ncl_json_as_int(value, &new_value) || no < 1 || no > 0xFFFF) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "要 keys（一个号）+ value（新值）");
            }
            rc = ncl_syntec_param_put(syntec, (unsigned)no, (int32_t)new_value);
            if (rc != NCL_OK) {
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_syntec_last_error(syntec));
            }
            out = ncl_json_new_object();
            if (out == NULL) {
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            {
                char name[16];

                snprintf(name, sizeof(name), "%d", (int)no);
                (void)ncl_json_obj_set_int(out, name, new_value);
            }
            *result = out;
            return NCL_OK;
        }
        /* 也可以直接给字典：params 本身就是 {"321":100,...} */
        out = ncl_json_new_object();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < ncl_json_obj_len(params); i++) {
            const char *name = ncl_json_obj_key_at(params, i);
            long long no = 0;
            long long new_value = 0;

            if (name == NULL || strcmp(name, "operation") == 0 ||
                strcmp(name, "keys") == 0 || strcmp(name, "check") == 0 ||
                strcmp(name, "token") == 0 || strcmp(name, "async") == 0) {
                continue; /* 这几个是框架自己的成员 */
            }
            if (i >= SYNTEC_PARAM_BATCH_MAX) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                     (unsigned)SYNTEC_PARAM_BATCH_MAX);
            }
            if (!ncl_json_as_int(ncl_json_obj_get(params, name), &new_value) ||
                sscanf(name, "%lld", &no) != 1 || no < 1 || no > 0xFFFF) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "写法是 {参数号:新值} 或 {\"keys\":...,\"value\":...}");
            }
            rc = ncl_syntec_param_put(syntec, (unsigned)no, (int32_t)new_value);
            if (rc != NCL_OK) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_syntec_last_error(syntec));
            }
            (void)ncl_json_obj_set_int(out, name, new_value);
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "参数只答 get_length / get_keys / get_value / get_attributes / set_value");
    }
}


/* ---------------------------------------------------------------- 刀具 ---- */

/** 一条刀补 -> 元素 JSON（§11.7）：册 4 的 `id/kind/radius/length` 在前，SYNTEC 特有的在后。 */
static ncl_json *syntec_tool_json(const ncl_syntec_tool *tool, unsigned no)
{
    ncl_json *obj = ncl_json_new_object();
    ncl_json *geometry = ncl_json_new_array();
    ncl_json *wear = ncl_json_new_array();
    size_t i;

    if (obj == NULL || geometry == NULL || wear == NULL) {
        ncl_json_free(obj);
        ncl_json_free(geometry);
        ncl_json_free(wear);
        return NULL;
    }
    (void)ncl_json_obj_set_int(obj, "id", (long long)no);
    /* kind ← 刀尖号：册 4 的 kind 原文注着"需要再确认"，这是最接近的一项 */
    (void)ncl_json_obj_set_int(obj, "kind", tool->tool_nose);
    (void)ncl_json_obj_set_double(obj, "radius", tool->radius_geometry);
    (void)ncl_json_obj_set_double(obj, "length", tool->length_geometry[0]);
    (void)ncl_json_obj_set_double(obj, "tool_angle", tool->tool_angle);
    (void)ncl_json_obj_set_double(obj, "radius_wear", tool->radius_wear);
    for (i = 0; i < NCL_SYNTEC_TOOL_LENGTHS; i++) {
        (void)ncl_json_arr_push(geometry,
                                ncl_json_new_double(tool->length_geometry[i]));
        (void)ncl_json_arr_push(wear, ncl_json_new_double(tool->length_wear[i]));
    }
    (void)ncl_json_obj_set(obj, "length_geometry", geometry);
    (void)ncl_json_obj_set(obj, "length_wear", wear);
    return obj;
}

/**
 * 写刀补时把 value 里的字段盖到一条刀补上（key 定刀号，字段按上面的元数据）。
 *
 * 只认上面 get_attributes 报出来的那些名字，多一个就报错：CNC 的模型不想改来
 * 改去，写错了名字（"radiuswear"）应该当场被拒，而不是悄悄什么都没写。
 * `id` 是只读的，给了也照收（读回来的对象直接改两个字段再写回去就行）。
 */
static ncl_err syntec_tool_patch(ncl_syntec_tool *tool, const ncl_json *value,
                                 char **reason)
{
    size_t n = ncl_json_obj_len(value);
    size_t i;

    if (n == 0) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                             "value 要是刀补对象，至少给一个字段");
    }
    for (i = 0; i < n; i++) {
        const char *name = ncl_json_obj_key_at(value, i);
        const ncl_json *item = ncl_json_obj_val_at(value, i);
        size_t k;

        if (name == NULL || item == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "value 里有坏字段");
        }
        if (strcmp(name, "id") == 0) {
            continue; /* 刀号由 key 定 */
        }
        if (strcmp(name, "kind") == 0 || strcmp(name, "tool_nose") == 0) {
            long long nose = 0;

            /* 线上 ToolNose 是 i16，超了会在控制器那头被截断 */
            if (!ncl_json_as_int(item, &nose) || nose < -32768 || nose > 32767) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "kind 要是 -32768..32767 的整数");
            }
            tool->tool_nose = (int32_t)nose;
            continue;
        }
        if (strcmp(name, "radius") == 0) {
            if (!ncl_json_as_double(item, &tool->radius_geometry)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "radius 要是数");
            }
            continue;
        }
        if (strcmp(name, "radius_wear") == 0) {
            if (!ncl_json_as_double(item, &tool->radius_wear)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "radius_wear 要是数");
            }
            continue;
        }
        if (strcmp(name, "tool_angle") == 0) {
            if (!ncl_json_as_double(item, &tool->tool_angle)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "tool_angle 要是数");
            }
            continue;
        }
        if (strcmp(name, "length") == 0) { /* length = 长度几何第 0 组 */
            if (!ncl_json_as_double(item, &tool->length_geometry[0])) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "length 要是数");
            }
            continue;
        }
        if (strcmp(name, "length_geometry") == 0 ||
            strcmp(name, "length_wear") == 0) {
            double *dst = strcmp(name, "length_geometry") == 0
                              ? tool->length_geometry
                              : tool->length_wear;

            if (ncl_json_arr_len(item) != NCL_SYNTEC_TOOL_LENGTHS) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "%s 要正好 %u 个数", name,
                                     (unsigned)NCL_SYNTEC_TOOL_LENGTHS);
            }
            for (k = 0; k < NCL_SYNTEC_TOOL_LENGTHS; k++) {
                if (!ncl_json_as_double(ncl_json_arr_get(item, k), &dst[k])) {
                    return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                         "%s 第 %u 个不是数", name,
                                         (unsigned)(k + 1));
                }
            }
            continue;
        }
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "不认识的字段 %s", name);
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ PLC / 变量 -- */

/**
 * `/CONTROLLER/REGISTER@<族>`：PLC 的寄存器与位（§11.9）。**一个族一条**，族在 `number` 上：
 *
 *   REGISTER@R   R 寄存器（u32）  0 .. 65535（21A）
 *   REGISTER@I   I 位（输入）    0 .. 511
 *   REGISTER@O   O 位（输出）
 *   REGISTER@C   C 位（新代自己的叫法，不是计数器）
 *   REGISTER@S   S 位
 *   REGISTER@A   A 位
 *
 * 取值形状是一段**按号排的表**（LIST）：答 `get_length`（这一族有多少个），
 * 元素按号取（`keys` 给号，0 起），所以不答 `get_keys`。定时器/计数器
 * （`PlcGetTimer`/`Counter`）是四个字段的结构体，先不进这一格。
 *
 * 只答读：位写（`0x0413` 等）与寄存器写（`0x041B`）控制器侧有，权限在适配器外面，
 * 这一轮没开。
 */
static uint32_t syntec_register_slots(const ncl_syntec_plc_slots *slots,
                                      char family)
{
    switch (family) {
    case 'R':
        return slots->registers;
    case 'I':
        return slots->ibits;
    case 'O':
        return slots->obits;
    case 'C':
        return slots->cbits;
    case 'S':
        return slots->sbits;
    case 'A':
        return slots->abits;
    default:
        return 0;
    }
}

/** 族 -> 位族的枚举（R 不是位，调用前先分开）。 */
static ncl_syntec_plc_kind syntec_register_kind(char family)
{
    switch (family) {
    case 'I':
        return NCL_SYNTEC_PLC_I;
    case 'O':
        return NCL_SYNTEC_PLC_O;
    case 'C':
        return NCL_SYNTEC_PLC_C;
    case 'S':
        return NCL_SYNTEC_PLC_S;
    default:
        return NCL_SYNTEC_PLC_A;
    }
}

static ncl_err syntec_register_table(void *ctx, const ncl_tool_point *self,
                                     ncl_operation op, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    const char *family_text = (const char *)self->arg; /* 声明里给的那一族 */
    ncl_syntec_plc_slots slots;
    char family;
    uint32_t count;
    ncl_err rc;
    size_t i;

    if (family_text == NULL || family_text[0] == '\0') {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "这条点位没带族（@R/@I…）");
    }
    family = family_text[0];
    if (family >= 'a' && family <= 'z') {
        family = (char)(family - 'a' + 'A');
    }
    rc = ncl_syntec_plc_capacity(syntec, &slots);
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
    }
    count = syntec_register_slots(&slots, family);
    if (count == 0) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                             "不认识的族 %c（R/I/O/C/S/A）", family);
    }
    switch (op) {
    case NCL_OP_GET_LENGTH: /* LIST：这一族有多少个 */
        return ncl_tool_reply_int(result, (long long)count);
    case NCL_OP_GET_ATTRIBUTES: {
        ncl_json *array = ncl_json_new_array();
        const char *const fields[][2] = {
            {"number", "号（从 0 起，就是 keys）"},
            {"value", family == 'R' ? "寄存器值（u32）" : "位（true/false）"},
            {"count", "这一族有多少个（= get_length）"},
        };

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            ncl_json *entry = ncl_json_new_object();

            if (entry == NULL ||
                ncl_json_obj_set_string(entry, "name", fields[i][0]) != NCL_OK ||
                ncl_json_obj_set_string(entry, "meaning", fields[i][1]) != NCL_OK ||
                ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(array);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE: {
        size_t n = syntec_param_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) { /* 空 keys：不猜，答空的 */
            *result = out;
            return NCL_OK;
        }
        if (n > SYNTEC_PARAM_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)SYNTEC_PARAM_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            long long no = 0;
            char name[24];

            if (!syntec_param_key(keys, i, &no) || no < 0 ||
                (unsigned long long)no >= (unsigned long long)count) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是 %c 族的号（0..%u）",
                                     (unsigned)(i + 1), family,
                                     (unsigned)(count - 1u));
            }
            if (family == 'R') {
                uint32_t value = 0;

                rc = ncl_syntec_plc_register(syntec, (unsigned)no, &value);
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
            } else {
                bool bit = false;

                rc = ncl_syntec_plc_bit(syntec, syntec_register_kind(family),
                                        (unsigned)no, &bit);
                if (rc != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, rc, "%s",
                                         ncl_syntec_last_error(syntec));
                }
                snprintf(name, sizeof(name), "%d", (int)no);
                if (ncl_json_obj_set_bool(out, name, bit) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: { /* §11.9：R 写 u32、I/C/S 写位（O 只能 Force、A 没有写） */
        const ncl_json *value = ncl_params_get(params, "value");
        long long no = 0;
        ncl_json *out;
        char name[24];

        if (!syntec_param_key(keys, 0, &no) || no < 0 ||
            (unsigned long long)no >= (unsigned long long)count) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=%c 族的一个号（0..%u）", family,
                                 (unsigned)(count - 1u));
        }
        if (value == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "要 value=新值");
        }
        if (family == 'R') {
            long long raw = 0;

            if (!ncl_json_as_int(value, &raw) || raw < 0 ||
                (unsigned long long)raw > 0xFFFFFFFFull) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "R 是 32 位无符号，value 给整数");
            }
            rc = ncl_syntec_plc_register_put(syntec, (unsigned)no,
                                             (uint32_t)raw);
            if (rc != NCL_OK) {
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_syntec_last_error(syntec));
            }
        } else {
            bool bit = false;

            if (!ncl_json_as_bool(value, &bit)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "位只能写 true / false");
            }
            rc = ncl_syntec_plc_bit_put(syntec, syntec_register_kind(family),
                                        (unsigned)no, bit);
            if (rc != NCL_OK) {
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_syntec_last_error(syntec));
            }
        }
        /*
         * 答"我写下去的值"（和参数写一样）。控制器认不认要**自己读回来看**：
         * 这台 21A 上 I 位由现场/梯形图驱动（写了读回来还是 0）、个别号
         * （#499/#500）也是写不动的 —— 帧没错，是控制器那边不接。
         */
        out = ncl_json_new_object();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        snprintf(name, sizeof(name), "%d", (int)no);
        if (ncl_json_obj_set(out, name, ncl_json_clone(value)) != NCL_OK) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "寄存器表答 get_length / get_value / set_value / get_attributes");
    }
}
/**
 * `/CONTROLLER/VARIABLE`：变量表（§11.9），册 4 表 7 的 `VARIABLE`（list，归 configs）。
 *
 * key 是变量号（就是程序里的 `#号`），值按控制器的类型给整数或浮点；控制器说"空"
 * 的号（`nValType` 0/3）答 `{"id":n,"type":0}`，不硬凑一个 0 出来。
 * 21A：容量 14096。
 */
static ncl_err syntec_variable_table(void *ctx, const ncl_tool_point *self,
                                     ncl_operation op, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    size_t count = 0;
    size_t i;
    ncl_err rc;

    (void)self;
    switch (op) {
    case NCL_OP_GET_LENGTH:
        rc = ncl_syntec_variable_capacity(syntec, &count);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        return ncl_tool_reply_int(result, (long long)count);
    case NCL_OP_GET_ATTRIBUTES: {
        static const char *const k_fields[][2] = {
            {"id", "变量号（就是程序里的 #号）"},
            {"value", "值：整数或浮点"},
            {"type", "控制器说的类型：0 空、1 整数、2 浮点"},
        };
        ncl_json *array = ncl_json_new_array();

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
            ncl_json *entry = ncl_json_new_object();

            if (entry == NULL ||
                ncl_json_obj_set_string(entry, "name", k_fields[i][0]) != NCL_OK ||
                ncl_json_obj_set_string(entry, "meaning", k_fields[i][1]) != NCL_OK ||
                ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(array);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE: {
        size_t n = syntec_param_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) {
            *result = out;
            return NCL_OK;
        }
        if (n > SYNTEC_PARAM_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)SYNTEC_PARAM_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            ncl_syntec_variant value;
            long long no = 0;
            char name[24];
            ncl_json *entry;

            if (!syntec_param_key(keys, i, &no) || no < 0 || no > 0xFFFF) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是变量号", (unsigned)(i + 1));
            }
            rc = ncl_syntec_variable(syntec, (unsigned)no, &value);
            if (rc != NCL_OK) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_syntec_last_error(syntec));
            }
            entry = ncl_json_new_object();
            if (entry == NULL) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            (void)ncl_json_obj_set_int(entry, "id", no);
            (void)ncl_json_obj_set_int(entry, "type", value.type);
            if (value.type == 1) {
                (void)ncl_json_obj_set_int(entry, "value", value.int_value);
            } else if (value.type == 2) {
                (void)ncl_json_obj_set_double(entry, "value", value.double_value);
            }
            snprintf(name, sizeof(name), "%d", (int)no);
            if (ncl_json_obj_set(out, name, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: { /* §11.9：写变量（0x0422）。整数写 INT 变体，带小数点/指数的写 DOUBLE */
        const ncl_json *value = ncl_params_get(params, "value");
        ncl_syntec_variant variant;
        const char *raw;
        double dbl = 0.0;
        long long no = 0;
        ncl_json *out;
        char name[24];

        if (!syntec_param_key(keys, 0, &no) || no < 0 || no > 0xFFFF) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=一个变量号（程序里的 #号）");
        }
        if (value == NULL || !ncl_json_as_double(value, &dbl)) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "要 value=新值（数）");
        }
        memset(&variant, 0, sizeof(variant));
        /*
         * 整数还是浮点：先看字面量里有没有小数点/指数（"2.5" 与 "2" 分得开），
         * 程序里 new_double(2.5) 这种没有字面量的，就看值本身有没有小数部分。
         */
        {
            long long as_int = 0;
            bool is_double = !(ncl_json_as_int(value, &as_int) &&
                               (double)as_int == dbl);

            raw = ncl_json_number_raw(value);
            if (raw != NULL) {
                is_double = strpbrk(raw, ".eE") != NULL;
            }
            if (is_double) {
                variant.type = 2; /* 控制器侧也有 DOUBLE 变体 */
                variant.double_value = dbl;
                variant.int_value = (int32_t)dbl;
            } else {
                if (dbl > 2147483647.0 || dbl < -2147483648.0) {
                    return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                         "整数变体是 32 位，超了就用浮点写");
                }
                variant.type = 1;
                variant.int_value = (int32_t)dbl;
                variant.double_value = dbl;
            }
        }
        rc = ncl_syntec_variable_put(syntec, (unsigned)no, &variant);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        /* 答我写下去的值；控制器认不认要读回来才知道（见寄存器那条注释）。 */
        out = ncl_json_new_object();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        snprintf(name, sizeof(name), "%d", (int)no);
        if (ncl_json_obj_set(out, name, ncl_json_clone(value)) != NCL_OK) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "变量表答 get_length / get_value / set_value / get_attributes");
    }
}

/**
 * `/CONTROLLER/TOOL`：刀具表（§11.7）。刀号即 key（从 1 起），元素就是那把刀的刀补。
 *
 * 读：`0x04C2` 问条数、`0x043F` 读一条（224 字节）。
 * 写：`0x0440` 一条 **256 字节**的帧 —— 16 字节桩头后面直接跟 228 字节的
 * `{ nToolNo, TToolOffset }`。set_value 先读回当前值打底、再让给到的字段覆盖，
 * 所以只想改一个磨损值就只给那一个字段。
 */
static ncl_err syntec_tool_table(void *ctx, const ncl_tool_point *self,
                                 ncl_operation op, const ncl_json *params,
                                 ncl_json **result, char **reason)
{
    ncl_syntec *syntec = (ncl_syntec *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    size_t count = 0;
    size_t i;
    ncl_err rc;

    (void)self;
    switch (op) {
    case NCL_OP_GET_LENGTH:
        rc = ncl_syntec_tool_count(syntec, &count);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        return ncl_tool_reply_int(result, (long long)count);

    case NCL_OP_GET_ATTRIBUTES: { /* 元素里有哪些字段（不用问控制器） */
        static const char *const k_fields[][2] = {
            {"id", "刀具编号（就是 key）"},
            {"kind", "种类：这里取刀尖号 ToolNose（册 4 的 kind 待再确认）"},
            {"radius", "半径几何（RadiusGeometry）"},
            {"length", "长度几何第 0 组（LengthGeometry[0]）"},
            {"tool_angle", "刀尖角（ToolAngle）"},
            {"radius_wear", "半径磨损（RadiusWear）"},
            {"length_geometry", "长度几何 12 组（LengthGeometry[0..11]）"},
            {"length_wear", "长度磨损 12 组（LengthWear[0..11]）"},
            {"time_usage", "寿命：这条路上没有来源，不给（册 4 里 FANUC 走 cnc_rdlife）"},
        };
        ncl_json *array = ncl_json_new_array();

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
            ncl_json *entry = ncl_json_new_object();

            if (entry == NULL ||
                ncl_json_obj_set_string(entry, "name", k_fields[i][0]) != NCL_OK ||
                ncl_json_obj_set_string(entry, "meaning", k_fields[i][1]) != NCL_OK ||
                ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(array);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = array;
        return NCL_OK;
    }

    case NCL_OP_GET_VALUE: { /* 按刀号取值 */
        size_t n = syntec_param_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) { /* 盲读（轮询/自检）：答空的，不报错 */
            *result = out;
            return NCL_OK;
        }
        if (n > SYNTEC_PARAM_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 把刀",
                                 (unsigned)SYNTEC_PARAM_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            ncl_syntec_tool tool;
            ncl_json *entry;
            long long no = 0;
            char name[16];

            if (!syntec_param_key(keys, i, &no) || no < 1 || no > 0xFFFF) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 里第 %u 个不是刀号", (unsigned)(i + 1));
            }
            rc = ncl_syntec_tool_get(syntec, (unsigned)no, &tool); /* 刀号从 1 起，和线上一致 */
            if (rc != NCL_OK) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_syntec_last_error(syntec));
            }
            entry = syntec_tool_json(&tool, (unsigned)no);
            snprintf(name, sizeof(name), "%d", (int)no);
            if (entry == NULL || ncl_json_obj_set(out, name, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: /* 写刀补（§11.7 的 0x0440）；权限在适配器外面控 */
    {
        const ncl_json *value = ncl_params_get(params, "value");
        ncl_syntec_tool tool;
        long long no = 0;

        if (!syntec_param_key(keys, 0, &no) || no < 1 || no > 0xFFFF) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=一个刀号（从 1 起）");
        }
        if (value == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 value=一条刀补对象");
        }
        /*
         * 一次写整条记录（控制器侧的 NcPutToolCompensation 收的就是整条 224 字节），
         * 所以先读回当前值打底、再让给到的字段覆盖：只写一两个字段也不会把
         * 别的字段抹掉。
         */
        rc = ncl_syntec_tool_get(syntec, (unsigned)no, &tool);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        rc = syntec_tool_patch(&tool, value, reason);
        if (rc != NCL_OK) {
            return rc;
        }
        rc = ncl_syntec_tool_put(syntec, (unsigned)no, &tool);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_syntec_last_error(syntec));
        }
        {
            ncl_json *out = ncl_json_new_object();
            ncl_json *entry;
            char name[16];

            if (out == NULL) {
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            entry = syntec_tool_json(&tool, (unsigned)no);
            snprintf(name, sizeof(name), "%d", (int)no);
            if (entry == NULL || ncl_json_obj_set(out, name, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            *result = out;
        }
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "刀具表只答 get_length / get_keys / get_value / get_attributes");
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

NCL_TOOL_BEGIN("syntec", "SYNTEC RemoteCNC over TCP (8000)", "MACHINE",
               1000, 1000, syntec_open, syntec_close)

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
     * 写：set_value（§11.6），权限在适配器外面控。 */
    /* 取值形状是 dict（HASH）：答 get_keys（号清单），不答 get_length。 */
    NCL_CONFIG_OPS("/CONTROLLER/PARAMETER", syntec_parameter, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_KEYS) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

    /* 刀具表（§11.7）：一条 config，元素就是那把刀的刀补（TOOLPARAM 是 TOOL 的元素）。
     * 读（0x04C2 条数 / 0x043F 一条）与写（0x0440，整条 224 字节）都在；
     * 写权限在适配器外面控（"权限在外面"），这里只提供能力。 */
    /* 取值形状是 list（LIST）：答 get_length（有几把），不答 get_keys。 */
    NCL_CONFIG_OPS("/CONTROLLER/TOOL", syntec_tool_table, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

    /* PLC（§11.9）：REGISTER 一类、**一族一条**，族在 number 上（@R / @I / …）。
     * 取值形状是按号排的表（LIST）：答 get_length，不答 get_keys。只答读。 */
/* R 与 I/C/S 能写（0x041B / 0x0413/16/18）；O 只能 Force、A 没有写，后两条只给读。 */
#define SYNTEC_REGISTER_OPS                                                     \
    (NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_GET_LENGTH) |            \
     NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))
#define SYNTEC_REGISTER_RW_OPS                                                  \
    (SYNTEC_REGISTER_OPS | NCL_OP_BIT(NCL_OP_SET_VALUE))
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@R", syntec_register_table, "R",
                   SYNTEC_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@I", syntec_register_table, "I",
                   SYNTEC_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@O", syntec_register_table, "O",
                   SYNTEC_REGISTER_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@C", syntec_register_table, "C",
                   SYNTEC_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@S", syntec_register_table, "S",
                   SYNTEC_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@A", syntec_register_table, "A",
                   SYNTEC_REGISTER_OPS)
#undef SYNTEC_REGISTER_OPS
    /* 变量表（§11.9）：册 4 表 7 的 VARIABLE（list，configs），key = 变量号（#号）。 */
    /* 变量表是 list（LIST）：答 get_length（多少个号），不答 get_keys。 */
    NCL_CONFIG_OPS("/CONTROLLER/VARIABLE", syntec_variable_table, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

NCL_TOOL_END_WITH_RAW(syntec_last_raw)

NCL_TOOL_MODULE("1.0.0",
                "新代 SYNTEC（RemoteCNC）适配器，九项按 10 册 §3.1/§3.2 的现场闭环实现")
