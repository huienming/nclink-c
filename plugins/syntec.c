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

/** §6 的审计要原始报文：问 client 一句就够，账由宿主管。 */
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

    NCL_METHOD_CALL("/SESSION", syntec_session)

NCL_TOOL_END_WITH_RAW(syntec_last_raw)

NCL_TOOL_MODULE("1.0.0",
                "新代 SYNTEC（RemoteCNC）适配器，九项按 10 册 §3.1/§3.2 的现场闭环实现")
