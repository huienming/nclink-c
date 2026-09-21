/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FANUC 适配器 —— 一个文件一台机床。
 *
 * 这份文件只做两件事，从上往下读一遍，就知道这台机床对外有哪些量：
 *
 *   1. **绑定**：把 client 的语义函数挂到模型路径上，一行一个点。
 *      "RDCOUNT 就是加工件数"、"STATUS 由 ODBST 的哪两位推出来"这些知识都在
 *      client 那一边（nclink/clients/focas.h），这里不重复。
 *   2. **覆盖**：两个现场调试方法自己写一小段；帧还没抓到的点也照常绑定 —— 那几
 *      个 client 函数现在回 NCL_ERR_UNAVAILABLE（"还读不了"），抓包补上只改 client，
 *      这张表一行都不用动。
 *
 * 协议细节（PDU 帧、item 码、回复块布局）在这份文件里一个字都没有 —— 那是 client
 * 的事，现场不需要懂。
 *
 * 每个名字都来自数据字典（册 32 第 4 部分）：STATUS / PART_COUNT / WARNING /
 * PROGRAM / POSITION / SPEED。FANUC 自己的 ODBST 位域不进模型 —— 字典里没有那些
 * 名字 —— 所以派生出来的 STATUS 用的是字典里的名字。
 *
 * 默认采样通道按现场口径只放四样：设备状态、加工计件、程序名称、报警。位置、速度
 * 一律按需读 —— 要上报就把那一行的 NCL_DATAITEM_F64 换成 NCL_DATAITEM_F64_SAMPLED。
 */
#include "nclink/ncl_tool.h"

#include "nclink/clients/focas.h"
#include "nclink/ncl_json.h"

/* ---------------------------------------------------------------- 连接 ---- */

/**
 * 把配置里的 parameters 交给 client。会话在第一次读的时候才建，所以机床没开机
 * 不影响设备程序启动：谁问值，谁拿到一条明确的"读不到"。
 */
static void *focas_open(const ncl_json *params, char **err)
{
    ncl_focas_config config;

    ncl_focas_config_default(&config);
    config.host = ncl_tool_param_str(params, "host", "");
    config.port =
        (unsigned)ncl_tool_param_int(params, "port", (long long)config.port);
    config.timeout_ms = (unsigned)ncl_tool_param_int(params, "timeoutMs",
                                                     (long long)config.timeout_ms);
    config.connect_timeout_ms =
        (unsigned)ncl_tool_param_int(params, "connectTimeoutMs",
                                     (long long)config.connect_timeout_ms);
    config.retries = (unsigned)ncl_tool_param_int(params, "retries",
                                                  (long long)config.retries);
    config.negotiate =
        ncl_tool_param_bool(params, "negotiate", config.negotiate);
    return ncl_focas_open(&config, err);
}

static void focas_close(void *ctx)
{
    ncl_focas_close((ncl_focas *)ctx);
}

/** §6 的审计要原始报文：问 client 一句就够，账由宿主管。 */
static void focas_last_raw(void *ctx, ncl_tool_frames *out)
{
    ncl_driver_raw raw;

    ncl_focas_last_raw((ncl_focas *)ctx, &raw);
    out->request = raw.request;
    out->request_len = raw.request_len;
    out->reply = raw.reply;
    out->reply_len = raw.reply_len;
}

/* -------------------------------------------------------------- 覆盖档 ---- */

/*
 * 两个现场调试方法（不进模型、不参与采样）：会话现在什么样、client 的 item 表里
 * 有什么。它们只是把 client 的诊断操作转出去，失败时把原因带上 —— 这就是"覆盖"
 * 的样子：需要自己解释时才写函数，而函数里照样用同一个 client 实例。
 */
static ncl_err session_method(void *ctx, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    ncl_err rc = ncl_focas_call(focas, "session", params, result);

    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    return NCL_OK;
}

static ncl_err items_method(void *ctx, const ncl_json *params,
                            ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    ncl_err rc = ncl_focas_call(focas, "items", params, result);

    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    return NCL_OK;
}

/* ------------------------------------------------------------------ 工具 -- */

NCL_TOOL_BEGIN("focas", "FANUC FOCAS / Fwlib32 over TCP, read only", "MACHINE", 1000, 1000,
               focas_open, focas_close)

    /* 默认采样通道只放四样（现场口径）：设备状态、加工计件、程序名称、报警。 */
    NCL_DATAITEM_STR_SAMPLED("/STATUS", ncl_focas_status)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", ncl_focas_part_count)
    /* PROGRAM 属于 CONTROLLER 组件（表 2：组件对象）*/
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_focas_program_name)

    /* 报警（表 6 的 WARNING）：进默认采样通道。帧还没抓到（01 册 §2.3 的码表里没有
     * cnc_rdalmmsg2），所以 ncl_focas_alarm() 现在回 NCL_ERR_UNAVAILABLE：模型里有
     * 这条路径、问它答"还读不了"、采样那一列是 null、自检算"待抓包"而不是失败。
     * 抓包补上只改 client 里那个函数体，这一行不动。 */
    NCL_DATAITEM_JSON_SAMPLED("/WARNING", ncl_focas_alarm)

    /* 五轴的位置：每轴两个 —— 实际（REAL，读得到）与目标（CMD，帧待抓包，函数回
     * NCL_ERR_UNAVAILABLE），都按需读。名字从路径自动推：
     * /MACHINE/AXIS@X/POSITION@REAL -> AXIS_X.POSITION_REAL。 */
    NCL_DATAITEM_F64("/AXIS@X/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_C)
    NCL_DATAITEM_F64("/AXIS@X/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_C)

    NCL_DATAITEM_F64("/AXIS@X/SPEED", ncl_focas_axis_speed, NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/SPEED", ncl_focas_axis_speed, NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/SPEED", ncl_focas_axis_speed, NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/SPEED", ncl_focas_axis_speed, NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/SPEED", ncl_focas_axis_speed, NCL_FOCAS_AXIS_C)

    /* 刀具列表（表 7 的 TOOL，list）：FOCAS 侧是刀补表/刀具表那一族调用，帧还没核对
     * （32 册 §5 把它列在"待核"里），所以 ncl_focas_tool_list() 现在回
     * NCL_ERR_UNAVAILABLE —— 模型里有它、问它有明确答复、轮询跳过；核对完改的就是
     * client 里那个函数体。 */
    NCL_CONFIG_JSON("/CONTROLLER/TOOL", ncl_focas_tool_list)

    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_CALL("/SESSION", session_method)
    NCL_METHOD_CALL("/ITEMS", items_method)

NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.4.0", "FANUC FOCAS / Fwlib32 over TCP, read only (01 册 §2.1-§2.3，32 册数据项)")
