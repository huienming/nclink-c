/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FOCAS client - 现场接口（语义层，见 nclink/clients/focas.h 第一节）。
 *
 * 这一层把"这台机床怎么读"固定在 client 里：哪个 item、哪一块、什么类型、怎么由位域
 * 推出三态。以前这些知识散在每个站点的点位表里（"RDCOUNT" 是什么、STATINFO@2 是哪
 * 一位），现在只在这里写一次，现场只写"这个函数绑到那条路径"。
 *
 * 实现站在既有的驱动上（ncl_focas_create() + ncl_driver_read_one()）：帧、会话、
 * 错误分级还是那一套，这里只加语义。
 */
#include "nclink/clients/focas.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

#include "focas/ncl_focas_pdu.h"

/** 一个 FOCAS 会话：驱动 + 一句话的错误原因。 */
struct ncl_focas {
    ncl_driver *driver;
    char        error[160];
};

/** 记下这次失败（绑定的 NG 理由就是这一句）。 */
static ncl_err note(ncl_focas *focas, const char *what, ncl_err code)
{
    if (focas != NULL) {
        snprintf(focas->error, sizeof(focas->error), "%s: %s", what,
                 ncl_err_name(code));
    }
    return code;
}

/**
 * 记一句"要抓哪一帧"再回 NCL_ERR_UNAVAILABLE（last_error 里排障看得到）。这一份
 * client 里凡是对应的 FOCAS 调用还没核准的，都从这里出去：工具层把这条点位答成
 * "还读不了"、不进轮询、不进 §6 审计（见 ncl_common.h 里这个码）。
 */
static ncl_err not_yet(ncl_focas *focas, const char *what, const char *call)
{
    if (focas != NULL) {
        snprintf(focas->error, sizeof(focas->error), "%s：帧待抓包（%s）", what,
                 call);
    }
    return NCL_ERR_UNAVAILABLE;
}

void ncl_focas_config_default(ncl_focas_config *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->port = 8193; /* §2.1: FOCAS over Ethernet listens here */
    config->connect_timeout_ms = 3000;
    config->timeout_ms = 3000;
    config->retries = 0;
    config->negotiate = true;
}

ncl_focas *ncl_focas_open(const ncl_focas_config *config, char **err)
{
    ncl_focas *focas;
    const ncl_driver_ops *ops;
    ncl_json *params;
    ncl_err rc;

    if (config == NULL || ncl_str_is_blank(config->host)) {
        if (err != NULL) {
            *err = ncl_strdup("FOCAS 缺少 host 参数");
        }
        return NULL;
    }
    focas = (ncl_focas *)ncl_mem_calloc(1, sizeof(*focas));
    if (focas == NULL) {
        return NULL;
    }
    focas->driver = ncl_focas_create();
    if (focas->driver == NULL) {
        ncl_mem_free(focas);
        return NULL;
    }
    ops = ncl_driver_ops_of(focas->driver);
    if (ops == NULL || ops->create == NULL) {
        note(focas, "FOCAS 驱动不完整", NCL_ERR_STATE);
        ncl_focas_close(focas);
        return NULL;
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        ncl_focas_close(focas);
        return NULL;
    }
    (void)ncl_json_obj_set_string(params, "host", config->host);
    (void)ncl_json_obj_set_int(params, "port", (long long)config->port);
    (void)ncl_json_obj_set_int(params, "timeoutMs",
                               (long long)config->timeout_ms);
    (void)ncl_json_obj_set_int(params, "connectTimeoutMs",
                               (long long)config->connect_timeout_ms);
    (void)ncl_json_obj_set_int(params, "retries", (long long)config->retries);
    (void)ncl_json_obj_set_bool(params, "negotiate", config->negotiate);
    rc = ops->create(focas->driver, params);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        note(focas, "FOCAS 连接参数不合法", rc);
        if (err != NULL) {
            *err = ncl_strdup(focas->error);
        }
        ncl_focas_close(focas);
        return NULL;
    }
    return focas; /* 会话还没建：第一次读的时候才连 */
}

void ncl_focas_close(ncl_focas *focas)
{
    if (focas == NULL) {
        return;
    }
    if (focas->driver != NULL) {
        const ncl_driver_ops *ops = ncl_driver_ops_of(focas->driver);

        if (ops != NULL && ops->destroy != NULL) {
            ops->destroy(focas->driver);
        }
        focas->driver = NULL;
    }
    ncl_mem_free(focas);
}

bool ncl_focas_connected(const ncl_focas *focas)
{
    const ncl_driver_ops *ops;

    if (focas == NULL || focas->driver == NULL) {
        return false;
    }
    ops = ncl_driver_ops_of(focas->driver);
    return ops != NULL && ops->is_connected != NULL &&
           ops->is_connected(focas->driver);
}

const char *ncl_focas_last_error(const ncl_focas *focas)
{
    return focas != NULL ? focas->error : "";
}

ncl_err ncl_focas_read_item(ncl_focas *focas, const char *item, long long block,
                            int length, ncl_dtype dtype, ncl_json **value)
{
    ncl_address address;
    ncl_err rc;

    if (focas == NULL || focas->driver == NULL || item == NULL ||
        value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    memset(&address, 0, sizeof(address));
    address.area = item;
    address.offset = block;
    address.bit = -1; /* 块里的字段写在 item 名字里（"ACTF@8"） */
    address.length = length > 0 ? length : 1;
    address.dtype = dtype;
    rc = ncl_driver_read_one(focas->driver, &address, value);
    if (rc != NCL_OK) {
        ncl_json_free(*value);
        *value = NULL;
        return note(focas, item, rc);
    }
    return NCL_OK;
}

ncl_err ncl_focas_call(ncl_focas *focas, const char *operation,
                       const ncl_json *params, ncl_json **result)
{
    const ncl_driver_ops *ops;
    ncl_err rc;

    if (focas == NULL || focas->driver == NULL || ncl_str_is_blank(operation)) {
        return NCL_ERR_INVALID_ARG;
    }
    ops = ncl_driver_ops_of(focas->driver);
    rc = ops != NULL && ops->call != NULL
             ? ops->call(focas->driver, operation, params, result)
             : NCL_ERR_NOT_SUPPORTED;
    return rc == NCL_OK ? NCL_OK : note(focas, operation, rc);
}

void ncl_focas_last_raw(ncl_focas *focas, ncl_driver_raw *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (focas != NULL && focas->driver != NULL) {
        ncl_driver_last_raw(focas->driver, out);
    }
}

/* ------------------------------------------------------------------ 语义 -- */

/*
 * 状态：读块 0 载荷的前六个 int16（= ODBST 的 manual/run/edit/motion/mstb/emergency），
 * 由 RUN（下标 1）与 EMERGENCY（下标 5）两位决定三态（表 6/表 8：
 * running / free / holding，holding = 紧急保持）。
 *
 * 下标是拿"斜坡载荷 + 官方 SDK 填它自己的 ODBST 结构体"钉出来的（01 册 §2.3）：
 *   块 1 → ODBST.dummy、块 2 → ODBST.aut、**块 0 的载荷** → manual, run, edit, motion,
 *   mstb, emergency, alarm, spindle, oper。所以 STATINFO@0 的下标 1 是 run、5 是
 *   emergency —— 而 aut 根本不在块 0（它在块 2），mode 要另读一条。
 */
ncl_err ncl_focas_status(ncl_focas *focas, char *out, size_t cap)
{
    ncl_json *bits = NULL;
    long long running = 0;
    long long emergency = 0;
    ncl_err rc;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "STATINFO@0", 0, 6, NCL_DTYPE_INT16, &bits);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_as_int(ncl_json_arr_get(bits, 1), &running);    /* run       */
    (void)ncl_json_as_int(ncl_json_arr_get(bits, 5), &emergency);  /* emergency */
    ncl_json_free(bits);
    snprintf(out, cap, "%s",
             emergency != 0 ? "holding" : running != 0 ? "running" : "free");
    return NCL_OK;
}

/*
 * RDCOUNT 是 int32，直接交数值（表 7 的 PART_COUNT 是 number）。
 *
 * **值不在载荷 0 处，在 @20**（原来按 @0 读，是错的）。两处独立证据：
 *
 *   1. 官方 SDK 的 `cnc_rdcount` 对着一台"每个字节都可辨识"的假机床跑，出参
 *      `ODBTLIFE3.data` 落到载荷 **@20**、`datano` 落到 **@2**（工具
 *      tools/site-probe/focas_sdk_layout.py 自动反查出来的：第 j 字节的值就是 j，
 *      出参那一格的值得等于谁，就知道它是从哪儿来的）。同一族里 `cnc_rdlife`
 *      的 data 在 **@12** —— 两条不是一个偏移，别串用。
 *   2. 现场包里那份 Linux 实现 `libfwlib32.so` 的 `cnc_rdcount` 也是这么切的：
 *      取块 +0x10 处 4 字节 `bswap` 后**低 16 位**当 datano（即载荷 @2 的 BE16）、
 *      取块 +0x24 处 4 字节 `bswap` 当 data（块 +0x10 就是载荷起点 → 载荷 @20）。
 *      见 01 册 §2.6。
 *
 * item 名字后面的 `@20` 就是"载荷里从第 20 字节开始"（同一套写法见 RDPRG@2）。
 */
ncl_err ncl_focas_part_count(ncl_focas *focas, long long *value)
{
    ncl_json *json = NULL;
    ncl_err rc;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDCOUNT@20", 0, 1, NCL_DTYPE_INT32, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(json, value)) {
        ncl_json_free(json);
        return note(focas, "RDCOUNT", NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    return NCL_OK;
}

ncl_err ncl_focas_program_name(ncl_focas *focas, char *out, size_t cap)
{
    ncl_json *value = NULL;
    const char *text;
    ncl_err rc;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    /* EXEPRGNAME2 是 36 字节的长名字（01 册 §2.3）。 */
    rc = ncl_focas_read_item(focas, "EXEPRGNAME2", 0, 36, NCL_DTYPE_STRING,
                             &value);
    if (rc != NCL_OK) {
        return rc;
    }
    text = ncl_json_as_string(value);
    snprintf(out, cap, "%s", text != NULL ? text : "");
    ncl_json_free(value);
    return NCL_OK;
}

/**
 * 每轴/每主轴一个 float 的那两条（`cnc_actf` / `cnc_acts`）：item 名 + 第几个。
 * 载荷里从 `index * 4` 字节开始就是这一个的值（大端 float32）。
 */
static ncl_err per_unit_float(ncl_focas *focas, const char *item_name,
                              int index, int count, double *value)
{
    char item[32];
    ncl_json *json = NULL;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0 || index >= count) {
        return note(focas, item_name, NCL_ERR_RANGE);
    }
    snprintf(item, sizeof(item), "%s@%d", item_name, index * 4);
    rc = ncl_focas_read_item(focas, item, 0, 1, NCL_DTYPE_FLOAT32, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_double(json, value)) {
        ncl_json_free(json);
        return note(focas, item, NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    return NCL_OK;
}

/* 轴的实际进给速度 F（cnc_actf，0x24）：每轴一个 float。 */
ncl_err ncl_focas_axis_feedrate(ncl_focas *focas, ncl_focas_axis axis,
                                double *value)
{
    return per_unit_float(focas, "ACTF", (int)axis, (int)NCL_FOCAS_AXIS_COUNT,
                          value);
}

/* 主轴的实际转速 S（cnc_acts，0x25）：每个主轴一个 float。 */
ncl_err ncl_focas_spindle_speed(ncl_focas *focas, unsigned spindle,
                                double *value)
{
    return per_unit_float(focas, "ACTS", (int)spindle, (int)NCL_FOCAS_SPINDLE_MAX,
                          value);
}

/*
 * 状态里除了三态之外还有两个位：模式（aut / manual）与急停。读的是同一份 STATINFO
 * （三块合一次查询），所以点位再多也不多花报文。
 */
ncl_err ncl_focas_mode(ncl_focas *focas, char *out, size_t cap)
{
    ncl_json *bits = NULL;
    ncl_json *aut_json = NULL;
    long long aut = 0;
    long long manual = 0;
    ncl_err rc;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    /* aut 在**块 2**（单独一条 Cb，码 152 —— ODBST 里它在偏移 4，不在块 0 的载荷里），
     * manual 在块 0 载荷的下标 0。表 8 的 WORK_MODE 取值就这两个。 */
    rc = ncl_focas_read_item(focas, "STATINFO", 2, 1, NCL_DTYPE_INT16, &aut_json);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_as_int(aut_json, &aut);
    ncl_json_free(aut_json);
    rc = ncl_focas_read_item(focas, "STATINFO@0", 0, 1, NCL_DTYPE_INT16, &bits);
    if (rc != NCL_OK) {
        return rc;
    }
    /* 读 1 个 int16 时拿到的是**标量**（不是数组）—— 和 per_unit_float 一个约定。 */
    (void)ncl_json_as_int(bits, &manual);                       /* manual */
    ncl_json_free(bits);
    snprintf(out, cap, "%s",
             aut != 0 ? "auto" : manual != 0 ? "manual" : "other");
    return NCL_OK;
}

/**
 * ODBST 块 0 载荷里的一个 u16（下标 = @p index）：0/1 变成 bool。
 *
 * 急停是下标 5（原来的 10 是**字节**偏移，对着旧口径写的；块 0 载荷的第一个 u16 是
 * manual，不是 ODBST 的偏移 0）。
 */
static ncl_err status_bit(ncl_focas *focas, int offset, bool *on)
{
    ncl_json *bits = NULL;
    long long value = 0;
    ncl_err rc;

    if (on == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "STATINFO@0", 0, offset + 1, NCL_DTYPE_INT16,
                             &bits);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_as_int(ncl_json_arr_get(bits, offset), &value);
    ncl_json_free(bits);
    *on = value != 0;
    return NCL_OK;
}

ncl_err ncl_focas_emergency(ncl_focas *focas, bool *on)
{
    return status_bit(focas, 5, on); /* 块 0 载荷下标 5 = ODBST.emergency */
}

/* 报警状态位（cnc_alarm2，0x1a）：载荷 @0 的 BE32，0 = 无报警。 */
ncl_err ncl_focas_alarm_status(ncl_focas *focas, long long *bits)
{
    ncl_json *json = NULL;
    ncl_err rc;

    if (bits == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDALM", 0, 1, NCL_DTYPE_INT32, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(json, bits)) {
        ncl_json_free(json);
        return note(focas, "RDALM", NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    return NCL_OK;
}

/* 程序号（cnc_rdprgnum，0x1c）：一条应答两个 short —— @2 运行中、@6 主程序。 */
static ncl_err program_number(ncl_focas *focas, int offset, long long *value)
{
    char item[32];
    ncl_json *json = NULL;
    ncl_err rc;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    snprintf(item, sizeof(item), "RDPRG@%d", offset);
    rc = ncl_focas_read_item(focas, item, 0, 1, NCL_DTYPE_INT16, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(json, value)) {
        ncl_json_free(json);
        return note(focas, item, NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    return NCL_OK;
}

ncl_err ncl_focas_program_number(ncl_focas *focas, long long *value)
{
    return program_number(focas, 2, value);
}

ncl_err ncl_focas_main_program_number(ncl_focas *focas, long long *value)
{
    return program_number(focas, 6, value);
}

/* 子程序号（cnc_rdexecprog3 的 ODBEXEPRGINFO）：表 7 的 SUBPROGRAM。 */
ncl_err ncl_focas_subprogram_number(ncl_focas *focas, long long *value)
{
    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "子程序号", "cnc_rdexecprog3（ODBEXEPRGINFO）");
}

/* 当前刀具号（表 7 的 TOOL_NUMBER）：模态 T 码，cnc_rdgcode 一条里带 T/B/S/F。 */
ncl_err ncl_focas_tool_number(ncl_focas *focas, long long *value)
{
    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "当前刀具号", "cnc_rdgcode（模态 T 码）");
}

/* 程序行号（cnc_rdseqnum，0x1d）：载荷 @0 的 BE32；表 7 的 LINE_NUMBER 是文本。 */
ncl_err ncl_focas_line_number(ncl_focas *focas, char *out, size_t cap)
{
    ncl_json *json = NULL;
    long long value = 0;
    ncl_err rc;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDSEQ", 0, 1, NCL_DTYPE_INT32, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(json, &value)) {
        ncl_json_free(json);
        return note(focas, "RDSEQ", NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    snprintf(out, cap, "N%lld", value);
    return NCL_OK;
}

/* 刀具组数（cnc_rdngrp，0x4a）：载荷 @0 的 BE32。 */
ncl_err ncl_focas_tool_group_count(ncl_focas *focas, long long *value)
{
    ncl_json *json = NULL;
    ncl_err rc;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDNGROUP", 0, 1, NCL_DTYPE_INT32, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(json, value)) {
        ncl_json_free(json);
        return note(focas, "RDNGROUP", NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    return NCL_OK;
}

/* 时钟（cnc_rdtimer，0x120）：载荷 @0 分钟、@4 毫秒，都是 BE32。 */
ncl_err ncl_focas_timer(ncl_focas *focas, ncl_focas_timer_kind kind,
                        long long *seconds)
{
    /* type 走 Cb 的 d，所以表里一种时间一条（RDTIMER…RDTIMER4）。 */
    static const char *const k_items[] = { "RDTIMER", "RDTIMER1", "RDTIMER2",
                                           "RDTIMER3", "RDTIMER4" };
    const char *item;
    ncl_json *json = NULL;
    long long minute = 0;
    long long msec = 0;
    ncl_err rc;

    if (seconds == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)kind < 0 || (int)kind > (int)NCL_FOCAS_TIMER_FREE) {
        return note(focas, "RDTIMER", NCL_ERR_INVALID_ARG);
    }
    item = k_items[(int)kind];
    rc = ncl_focas_read_item(focas, item, 0, 2, NCL_DTYPE_INT32, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_as_int(ncl_json_arr_get(json, 0), &minute);
    (void)ncl_json_as_int(ncl_json_arr_get(json, 1), &msec);
    ncl_json_free(json);
    *seconds = minute * 60 + msec / 1000;
    return NCL_OK;
}

/* ------------------------------------------------------- 还没抓到帧的调用 -- */

/*
 * 下面这些现场要的量，对应的 FOCAS 调用的**请求码已经核出来了**（见 focas_codec.c
 * 的 item 表与 01 册 §2.3），但**应答的切法还没在真机上核准**：值不在载荷 0 处、
 * 或者是结构体数组（位置、负载、报警消息、刀补、宏变量、参数…）。它们先按现场要
 * 的样子摆在这里，回 NCL_ERR_UNAVAILABLE：适配器照常把它们绑到模型路径上，于是那些
 * 点位在模型里看得见、问它答"还读不了"、轮询与 §6 审计都不碰、自检算成"待抓包"而
 * 不是失败（见 ncl_common.h 里这个码）。
 *
 * 为什么"没实现"也写在 client 里：帧格式、item 名、要读哪几块是这一层的知识；在
 * 适配器里再发明一套"待抓包"的写法，除了重复一句理由什么也表达不了，而且抓包补上
 * 时得改两处（点位表 + client）；现在只改下面的函数体，点位表一行都不用动。
 */

ncl_err ncl_focas_alarm(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    /* 帧补上之后：cnc_rdalmmsg2（item 0x23，d = 报警类型，e = 条数）按 ODBALMMSG2
     * 数组切，一条报警一个 {"number","text"}；形状见表 9。 */
    return not_yet(focas, "报警", "cnc_rdalmmsg2（item 0x23，31 册 §1 #8）");
}

/*
 * 坐标：`cnc_rdposition` 一条请求拿四路（绝对/机械/相对/剩余），每轴一个
 * **POSELM**（12 字节）：`int32 data` + `dec` + `unit` + `disp` + 轴名 + 后缀。
 * 请求的 9 个块与应答的 9 个块一一对应（§2.3 的约定），所以
 *   块 1 = 绝对、块 2 = 机械、块 3 = 相对、块 4 = 剩余（0 基下标）。
 * 位置值 = `data / 10^dec`（NCGuide 上实测 dec=3、轴名 'X'）。
 */
#define FOCAS_POSELM_SIZE 12

/** 从"整块载荷的字节数组"里取一个 POSELM 的字段（大端，与线上一致）。 */
static uint8_t bytes_at(const ncl_json *bytes, size_t index)
{
    long long value = 0;

    (void)ncl_json_as_int(ncl_json_arr_get((ncl_json *)bytes, index), &value);
    return (uint8_t)(value & 0xFF);
}

static bool poselm_read(const ncl_json *payload, size_t axis, int32_t *data,
                        int *dec)
{
    size_t at = axis * FOCAS_POSELM_SIZE;

    if (payload == NULL || ncl_json_type_of((ncl_json *)payload) != NCL_JSON_ARRAY ||
        ncl_json_arr_len((ncl_json *)payload) < at + FOCAS_POSELM_SIZE) {
        return false;
    }
    *data = (int32_t)(((uint32_t)bytes_at(payload, at) << 24) |
                      ((uint32_t)bytes_at(payload, at + 1) << 16) |
                      ((uint32_t)bytes_at(payload, at + 2) << 8) |
                      (uint32_t)bytes_at(payload, at + 3));
    *dec = (int)(((uint16_t)bytes_at(payload, at + 4) << 8) |
                 (uint16_t)bytes_at(payload, at + 5));
    return true;
}

/** 缩放到实际值：`data / 10^dec`（dec 是小数点位数）。 */
static double poselm_scale(int32_t data, int dec)
{
    double scale = 1.0;
    int i;

    for (i = 0; i < dec; i++) {
        scale *= 10.0;
    }
    return (double)data / scale;
}

/**
 * 位置那一路的公共读法：`which` 0..3 = 绝对/机械/相对/剩余（对应块 1..4）。
 * 一次请求把四路都取回来，多读一轴也只多花一次查询（本来一条请求就够）。
 *
 * `dec_out` 非空时把它带出来（`POSELM.dec` = 该轴的显示小数位）——伺服延迟量要按
 * 同一根轴的小数位缩放到实际值：官方手册说延迟量的小数位走 `cnc_getfigure`（该轴的
 * 显示小数位），而 POSELM 里带的就是同一个数，不用再问一趟。
 */
static ncl_err position_read_dec(ncl_focas *focas, ncl_focas_axis axis,
                                 int which, double *value, int *dec_out)
{
    ncl_json *payload = NULL;
    int32_t data = 0;
    int dec = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "RDPOSITION", NCL_ERR_RANGE);
    }
    /* 整块载荷按字节取回来（POSELM 是混合结构，驱动层只认单一 dtype）。 */
    rc = ncl_focas_read_item(focas, "RDPOSITION", 1 + which,
                             (int)(FOCAS_POSELM_SIZE * NCL_FOCAS_AXIS_COUNT),
                             NCL_DTYPE_BYTE, &payload);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!poselm_read(payload, (size_t)axis, &data, &dec)) {
        ncl_json_free(payload);
        return note(focas, "RDPOSITION", NCL_ERR_PARSE);
    }
    ncl_json_free(payload);
    *value = poselm_scale(data, dec);
    if (dec_out != NULL) {
        *dec_out = dec;
    }
    return NCL_OK;
}

static ncl_err position_read(ncl_focas *focas, ncl_focas_axis axis, int which,
                             const char *what, double *value)
{
    (void)what;
    return position_read_dec(focas, axis, which, value, NULL);
}

ncl_err ncl_focas_axis_position(ncl_focas *focas, ncl_focas_axis axis,
                                double *value)
{
    return position_read(focas, axis, 0, "绝对位置", value);
}

ncl_err ncl_focas_axis_position_machine(ncl_focas *focas, ncl_focas_axis axis,
                                        double *value)
{
    return position_read(focas, axis, 1, "机械坐标", value);
}

ncl_err ncl_focas_axis_position_relative(ncl_focas *focas,
                                         ncl_focas_axis axis, double *value)
{
    return position_read(focas, axis, 2, "相对坐标", value);
}

ncl_err ncl_focas_axis_distance(ncl_focas *focas, ncl_focas_axis axis,
                                double *value)
{
    return position_read(focas, axis, 3, "剩余距离", value);
}

/*
 * 伺服延迟量（`cnc_srvdelay`）= 现场说的**跟踪误差**。请求是一条 Cb（0x26，d = 9，
 * e = ALL_AXES，见 01 册 §2.4 的实测请求帧）；应答每轴一条 **8 字节记录**：
 *
 *   [0..4)  int32 data（大端）—— 延迟量（**检测单位**，见下）
 *   [4..8)  官方库不读这 4 个字节（下面 18005938c 那段只取第 0 个 dword）
 *
 * 依据（x64 以太网库 `fwlibe64.dll` 的 `cnc_srvdelay` → `sub_180059180`，2026-09 反汇编，
 * 和 32 位 HSSB 库 `fwlibNCG.dll` 的结论一致）：
 *
 *   180059321  shr ax, 3                     ; 轴数 = 载荷长度 / 8 → **每轴 8 字节**
 *   180059343  lea rcx, [rax*4 + 4]          ; 长度规则 = 4 + 4×轴数（不够回 EW_LENGTH）
 *   18005938c  mov ecx, [载荷 + i*8 + 0x10]  ; 取记录第 0 个 dword
 *   180059391  call bswap32                  ; 线上是大端
 *   180059396  mov [out + i*4 + 4], eax      ; → ODBAXIS.data[i]（type 在 +2、轴号）
 *
 * **小数位不在这条载荷里**（官方库一个字节都不多看）：手册说延迟量的小数位走
 * `cnc_getfigure`，也就是该轴的显示小数位 —— 我们直接拿同一根轴 POSELM 的 `dec`，
 * 不再多问一趟（见 srv_delay_raw()）。
 */
#define FOCAS_SVDEL_SIZE 8

static bool svdel_read(const ncl_json *payload, size_t axis, int32_t *data)
{
    size_t at = axis * FOCAS_SVDEL_SIZE;

    if (payload == NULL || ncl_json_type_of((ncl_json *)payload) != NCL_JSON_ARRAY ||
        ncl_json_arr_len((ncl_json *)payload) < at + FOCAS_SVDEL_SIZE) {
        return false;
    }
    *data = (int32_t)(((uint32_t)bytes_at(payload, at) << 24) |
                      ((uint32_t)bytes_at(payload, at + 1) << 16) |
                      ((uint32_t)bytes_at(payload, at + 2) << 8) |
                      (uint32_t)bytes_at(payload, at + 3));
    return true;
}

/** 读伺服延迟量并按给定的小数位缩放到实际值（`dec` 来自该轴的位置）。 */
static ncl_err srv_delay_raw(ncl_focas *focas, ncl_focas_axis axis, int dec,
                             double *value)
{
    ncl_json *payload = NULL;
    int32_t data = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "SV_DELAY", NCL_ERR_RANGE);
    }
    /* 这条 item 只有 1 个 Cb，所以应答也只有 1 个块——块号是**从 0 数**的
     * （和 STATINFO@0 一个约定），RDPOSITION 那条要的是下标 1。 */
    rc = ncl_focas_read_item(focas, "SV_DELAY", 0,
                             (int)(FOCAS_SVDEL_SIZE * NCL_FOCAS_AXIS_COUNT),
                             NCL_DTYPE_BYTE, &payload);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!svdel_read(payload, (size_t)axis, &data)) {
        ncl_json_free(payload);
        return note(focas, "SV_DELAY", NCL_ERR_PARSE);
    }
    ncl_json_free(payload);
    *value = poselm_scale(data, dec);
    return NCL_OK;
}

ncl_err ncl_focas_axis_srv_delay(ncl_focas *focas, ncl_focas_axis axis,
                                 double *value)
{
    double position = 0.0;
    int dec = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* 小数位跟位置一族走（cnc_getfigure 口径）：借同一条 POSELM 的 dec。 */
    rc = position_read_dec(focas, axis, 0, &position, &dec);
    if (rc != NCL_OK) {
        return rc;
    }
    return srv_delay_raw(focas, axis, dec, value);
}

/*
 * 指令位置（目标位置）。现场口径：**跟踪误差 = 实际位置 − 指令位置**，所以
 *
 *     指令位置 = 实际位置 − 伺服延迟量
 *
 * 两个量分两条读：实际位置走 `cnc_rdposition` 的绝对那一路（POSELM），伺服延迟量走
 * `cnc_srvdelay`（0x26 d = 9）。机床静止时延迟量是 0，"指令 = 实际"——这不是猜的，
 * NCGuide 上实测就是 0（§2.5）。
 */
ncl_err ncl_focas_axis_position_cmd(ncl_focas *focas, ncl_focas_axis axis,
                                    double *value)
{
    double actual = 0.0;
    double delay = 0.0;
    int dec = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "AXIS", NCL_ERR_RANGE);
    }
    /* 位置那趟顺带把该轴的小数位带出来，伺服延迟量按同一个位数缩放（cnc_getfigure 口径），
     * 这样两条量在同一口径上相减。 */
    rc = position_read_dec(focas, axis, 0, &actual, &dec);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = srv_delay_raw(focas, axis, dec, &delay);
    if (rc != NCL_OK) {
        return rc;
    }
    *value = actual - delay;
    return NCL_OK;
}

/* 伺服负载（cnc_rdsvmeter，0x56 + 0x89）：每轴一个 LOADELM（int32 + dec/unit/name）。 */
ncl_err ncl_focas_axis_load(ncl_focas *focas, ncl_focas_axis axis,
                            double *value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "SVLOAD", NCL_ERR_RANGE);
    }
    return not_yet(focas, "伺服负载", "cnc_rdsvmeter（item 0x56 + 0x89）");
}

/* 主轴负载/转速（cnc_rdspmeter，0x40：d=4 负载、d=5 转速；再跟一条 0x8a）。 */
ncl_err ncl_focas_spindle_load(ncl_focas *focas, unsigned spindle,
                               double *value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (spindle >= NCL_FOCAS_SPINDLE_MAX) {
        return note(focas, "SPLOAD", NCL_ERR_RANGE);
    }
    return not_yet(focas, "主轴负载", "cnc_rdspmeter（item 0x40 + 0x8a）");
}

/** 每轴一类量的公共壳（扭矩/电流/温度）：轴号先校验，再交回"还没实现"。 */
static ncl_err axis_measure_not_yet(ncl_focas *focas, ncl_focas_axis axis,
                                    double *value, const char *what,
                                    const char *call)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "AXIS", NCL_ERR_RANGE);
    }
    return not_yet(focas, what, call);
}

/* 表 4 的每轴物理量：同一个 cnc_rdaxisdata 一次能取多类（速度/负载/电流/温度）。 */
ncl_err ncl_focas_axis_torque(ncl_focas *focas, ncl_focas_axis axis,
                              double *value)
{
    return axis_measure_not_yet(focas, axis, value, "轴扭矩",
                                "cnc_loadtorq（ODBLOAD）");
}

ncl_err ncl_focas_axis_current(ncl_focas *focas, ncl_focas_axis axis,
                               double *value)
{
    return axis_measure_not_yet(focas, axis, value, "轴电流",
                                "cnc_rdaxisdata（电流那一类）");
}

ncl_err ncl_focas_axis_temperature(ncl_focas *focas, ncl_focas_axis axis,
                                   double *value)
{
    return axis_measure_not_yet(focas, axis, value, "轴温",
                                "cnc_rdaxisdata（温度那一类）");
}

/* 轴的种类（表 7 的 TYPE）：linear / rotary —— 读轴名与轴属性（cnc_rdaxisname）。 */
ncl_err ncl_focas_axis_type(ncl_focas *focas, ncl_focas_axis axis, char *out,
                            size_t cap)
{
    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "AXIS", NCL_ERR_RANGE);
    }
    out[0] = '\0';
    return not_yet(focas, "轴类型", "cnc_rdaxisname / cnc_rdaxisdata 的轴属性");
}

/*
 * 合成进给速度：`cnc_rddynamic2` 的 `ODBDY2.actf`（官方头里这条叫 `actf`，
 * 不叫 `feedrate`；而且 `length` 必须给 **sizeof(ODBDY2)**，它随轴数变）。
 * 单轴的进给速度已经能从 `cnc_actf` 拿到（见 ncl_focas_axis_feedrate）。
 */
ncl_err ncl_focas_feed_speed(ncl_focas *focas, double *value)
{
    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "合成进给速度", "cnc_rddynamic2（ODBDY2.actf，长度=sizeof(ODBDY2)）");
}

/*
 * 进给倍率：**不在** `cnc_rddynamic2` 里（`ODBDY2` 没有倍率字段），在操作面板信号
 * `IODBSGNL.feed_ovrd` 里 —— 走 `cnc_rdopnlsgnl`（Cb 0x5d，见 focas_codec.c 的
 * `RDSGNL` 那条）。载荷 @0xa 的 BE16 是**信号码**，官方文档把它换算成百分比写死了：
 *
 *     0 : 0%    5 : 50%    10 : 100%   15 : 150%   20 : 200%
 *     1 : 10%   6 : 60%    11 : 110%   16 : 160%
 *     ... 每级 10%（0..20 正好 0%..200%）
 *
 * 所以这里是 `码 × 10`，出门就是标准里的百分比。
 */
ncl_err ncl_focas_feed_override(ncl_focas *focas, double *value)
{
    ncl_json *json = NULL;
    long long code = 0;
    ncl_err rc;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDSGNL@10", 0, 1, NCL_DTYPE_INT16, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(json, &code)) {
        ncl_json_free(json);
        return note(focas, "RDSGNL", NCL_ERR_PARSE);
    }
    ncl_json_free(json);
    if (code < 0 || code > 20) {
        return note(focas, "RDSGNL", NCL_ERR_RANGE); /* 文档只定义 0..20 */
    }
    *value = (double)code * 10.0;
    return NCL_OK;
}

/*
 * 主轴倍率：`IODBSGNL.spdl_ovrd` 在**现代系列上是 "(Not used)"**（官方文档：只有
 * Series 15i 有这一格），16/18/21、16i/18i/21i、0i、30i、PMi-A 都没有。所以这条路
 * 不是"还没抓包"，是**这一格读不到**；要拿主轴倍率得走主轴数据那一族
 * （`cnc_rdspdata`）或读相关参数 —— 两者都还没核，先如实回"待抓包"。
 */
ncl_err ncl_focas_spindle_override(ncl_focas *focas, double *value)
{
    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "主轴倍率",
                   "IODBSGNL.spdl_ovrd 现代系列没有（cnc_rdspdata 或参数待核）");
}

/* 正在执行的程序段（cnc_rdexecprog）：应答里是"程序行文本"。 */
ncl_err ncl_focas_executed_block(ncl_focas *focas, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return not_yet(focas, "执行中的程序段", "cnc_rdexecprog");
}

/* 程序目录（cnc_rdprogdir3，0x06，d = 0x13）：PRGDIR3 数组，形状见 31 册 §1 #7。 */
ncl_err ncl_focas_program_directory(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "程序目录",
                   "cnc_rdprogdir3（item 0x06，d=0x13），32 册 §5 待核");
}

ncl_err ncl_focas_tool_list(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    /* 帧核对完：cnc_rdtooldata / cnc_rdtoolrng 两张表拼成一个 list。 */
    return not_yet(focas, "刀具列表",
                   "cnc_rdtooldata / cnc_rdtoolrng，32 册 §5");
}

/* 一条刀补（cnc_rdtofs，0x08）：形状/磨损 × 长度/半径，字段布局待真机核对。 */
ncl_err ncl_focas_tool_offset(ncl_focas *focas, long long index,
                              ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0) {
        return note(focas, "RDTOFS", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return not_yet(focas, "刀补", "cnc_rdtofs（item 0x08）");
}

/* 刀具寿命计数（cnc_rdlife，0x8b，d = e = 1）：值不在载荷 0 处，待真机核。 */
ncl_err ncl_focas_tool_life(ncl_focas *focas, long long group,
                            long long *value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (group < 0) {
        return note(focas, "RDLIFE", NCL_ERR_INVALID_ARG);
    }
    return not_yet(focas, "刀具寿命", "cnc_rdlife（item 0x8b，d=e=1）");
}

/* 一个用户宏变量（cnc_rdmacro，0x15）：值是"数值 + 小数位"，待核。 */
ncl_err ncl_focas_macro_variable(ncl_focas *focas, long long number,
                                 ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (number < 0) {
        return note(focas, "RDMACRO", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return not_yet(focas, "宏变量", "cnc_rdmacro（item 0x15）");
}

/* 一个 CNC 参数（cnc_rdparam，0x0e）：载荷里带参数号，布局待核。 */
ncl_err ncl_focas_parameter(ncl_focas *focas, long long number,
                            ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (number < 0) {
        return note(focas, "RDPARAM", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return not_yet(focas, "参数", "cnc_rdparam（item 0x0e）");
}

/* 工件坐标系（cnc_rdwkcdshft 一族）：G54… 的偏移表，帧待抓包。 */
ncl_err ncl_focas_work_offset(ncl_focas *focas, const char *name,
                              ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_str_is_blank(name)) {
        return note(focas, "WORK_OFFSET", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return not_yet(focas, "工件坐标系", "cnc_rdwkcdshft");
}

/* 当前模态（cnc_rdgcode）：T/B/S/F 与一组 G 代码，帧待抓包。 */
ncl_err ncl_focas_modal(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "模态", "cnc_rdgcode");
}

/*
 * 系统信息（cnc_sysinfo）：这一条**不发数据帧** —— 型号/系列/轴数在会话握手的
 * 应答里（`func 01` 的记录 + `func 21` 的 system_info 块）。要解那段记录才能给，
 * 现在的驱动只记了记录条数（`call("session")` 看得到）。
 */
ncl_err ncl_focas_system(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "系统信息", "cnc_sysinfo（会话握手记录）");
}

/* ------------------------------------------------------------ 程序上下行 -- */

/*
 * 程序的上下行不是"读一个 item"，是三件套（01 册 §2.4，官方 SDK 实测）：
 *
 *   下行（PC → CNC）  cnc_dwnstart4（func 0x11，定长 516 字节体：数据种类 +
 *                     目录名/程序名）→ 分块 cnc_download4（func 0x12、dir 4，
 *                     体就是程序文本）→ cnc_dwnend4（func 0x13；**下载的错误
 *                     都在这条上回**）
 *   上行（CNC → PC）  cnc_upstart4（0x15）→ cnc_upload4（0x18、dir 4）→ cnc_upend4
 *
 * 帧在驱动层（focas_driver.c 的 "download" / "upload" 操作），这里只管语义与
 * 参数；`type` 的取值照官方手册：0 NC 程序 / 1 刀补 / 2 参数 / 3 螺距误差 /
 * 4 宏变量 / 5 工件零点偏置。
 */

ncl_err ncl_focas_program_download(ncl_focas *focas, long long type,
                                   const char *dir, const char *program)
{
    ncl_json *params;
    ncl_json *result = NULL;
    ncl_err rc;

    if (focas == NULL || program == NULL || program[0] == '\0') {
        return NCL_ERR_INVALID_ARG;
    }
    if (type < 0 || type > 255) {
        return note(focas, "DWNSTART4", NCL_ERR_INVALID_ARG);
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_int(params, "type", type);
    if (!ncl_str_is_blank(dir)) {
        (void)ncl_json_obj_set_string(params, "dir", dir);
    }
    (void)ncl_json_obj_set_string(params, "data", program);
    rc = ncl_focas_call(focas, "download", params, &result);
    ncl_json_free(params);
    ncl_json_free(result);
    if (rc != NCL_OK) {
        return note(focas, "PROGRAM_DOWNLOAD", rc);
    }
    return NCL_OK;
}

/*
 * 上行（CNC → PC）：请求码 0x15（start，体同下行那 516 字节）/ 0x18（取一块，
 * 体 8 字节、dir 4）已经核出来了；**应答里程序文本的切法还没核** —— 官方库在
 * 内部函数里解（0x14fe70），反汇编到那一层没再往下，真机抓一次就能定。
 * 先照"还读不了"回：点位/方法在模型里看得见，问它有明确答复。
 */
ncl_err ncl_focas_program_upload(ncl_focas *focas, long long type,
                                 const char *name, char **program, size_t *len)
{
    if (focas == NULL || program == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    (void)type;
    (void)name;
    (void)len;
    *program = NULL;
    return not_yet(focas, "程序上传", "cnc_upload4（item 0x18 的应答切法）");
}

/* --------------------------------------------------- 表 7 的那几种表/对象 -- */

/* 一段宏变量（cnc_rdmacror，一次最多 5 个）：表 7 的 VARIABLE（list）。 */
ncl_err ncl_focas_macro_variables(ncl_focas *focas, long long first,
                                  long long count, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (first < 0 || count <= 0 || count > 5) {
        return note(focas, "RDMACROR", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return not_yet(focas, "宏变量表", "cnc_rdmacror（一次最多 5 个）");
}

/* 一套刀具参数（TOOLPARAM）：刀补（cnc_rdtofs）+ 寿命（cnc_rdlife）拼出来。 */
ncl_err ncl_focas_tool_param(ncl_focas *focas, long long index,
                             ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0) {
        return note(focas, "RDTOFS", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return not_yet(focas, "刀具参数",
                   "cnc_rdtofs（刀补）+ cnc_rdlife（寿命）");
}

/* 整张表的那三条（点位表里进 configs）：逐条拼，帧都还没核对。 */
ncl_err ncl_focas_tool_param_table(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "刀具参数表",
                   "cnc_rdtofs + cnc_rdlife 逐条读（一次一条）");
}

ncl_err ncl_focas_parameter_table(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "参数表",
                   "cnc_rdparanum + cnc_rdparar（item 0x0e 那一族）");
}

ncl_err ncl_focas_variable_table(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "宏变量表", "cnc_rdmacror（一次最多 5 个）");
}

ncl_err ncl_focas_work_offsets(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "工件坐标系", "cnc_rdwkcdshft 一族（G54…）");
}

/* 机床型号 / 系统版本：这两条的来源是会话握手那段记录（cnc_sysinfo）。 */
ncl_err ncl_focas_model(ncl_focas *focas, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return not_yet(focas, "机床型号", "cnc_rdmodel / cnc_sysinfo（握手记录）");
}

ncl_err ncl_focas_version(ncl_focas *focas, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return not_yet(focas, "系统版本", "cnc_sysinfo（series / version）");
}

/* 厂商：这一份 client 接的就是 FANUC，不用问机床（表 6 的 MANUFACTURER）。 */
ncl_err ncl_focas_manufacturer(ncl_focas *focas, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    (void)focas;
    snprintf(out, cap, "%s", "FANUC");
    return NCL_OK;
}

/* ------------------------------------------------------------- 写动作 -- */

/* 选主程序（cnc_pdf_slctmain）：现场调试方法，不进模型。 */
ncl_err ncl_focas_program_select_main(ncl_focas *focas, const char *name)
{
    if (focas == NULL || ncl_str_is_blank(name)) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "选主程序", "cnc_pdf_slctmain");
}

/* 删程序（cnc_delete / cnc_pdf_del）：机床会拒掉正在执行的程序。 */
ncl_err ncl_focas_program_delete(ncl_focas *focas, const char *name)
{
    if (focas == NULL || ncl_str_is_blank(name)) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "删程序", "cnc_delete / cnc_pdf_del");
}

/* 写参数（cnc_wrparam）：风险高，站点点位表里要用得想清楚。 */
ncl_err ncl_focas_parameter_write(ncl_focas *focas, long long number,
                                  const char *value)
{
    if (focas == NULL || ncl_str_is_blank(value) || number < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "写参数", "cnc_wrparam（写前先确认权限与备份）");
}

/* 写刀补（cnc_wrtofs）：**改刀补会导致撞刀**，官方手册专门警告过。 */
ncl_err ncl_focas_tool_offset_write(ncl_focas *focas, long long index,
                                    const char *value)
{
    if (focas == NULL || ncl_str_is_blank(value) || index < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    return not_yet(focas, "写刀补", "cnc_wrtofs（改刀补会导致撞刀）");
}

/* 写宏变量（cnc_wrmacro）。 */
ncl_err ncl_focas_macro_write(ncl_focas *focas, long long number, double value)
{
    if (focas == NULL || number < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    (void)value;
    return not_yet(focas, "写宏变量", "cnc_wrmacro");
}
