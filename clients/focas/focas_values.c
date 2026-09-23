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
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_charset.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_platform.h" /* ncl_sleep_millis：写参数的读回复核要轮询 */

#include "focas/ncl_focas_pdu.h"

/** 一个 FOCAS 会话：驱动 + 一句话的错误原因。 */
struct ncl_focas {
    ncl_driver *driver;
    char        error[160];
    /**
     * 这台机床**实际有几根轴**（读一次 `cnc_rdaxisname` 的表，记住）。0 = 还没读到。
     *
     * 为什么要它：适配器按 5 轴（X/Y/Z/A/C）声明点位，但真正接的机器可能只有 3 根 ——
     * 那时第 4、5 根轴的记录**根本不在应答里**（真机上读到的是载荷后面的填充/垃圾，
     * 数值能到 6e8）。所以每根轴先跟这个数比一下：没有那根轴就如实回"没有"，
     * 而不是把垃圾当坐标报出去。
     */
    size_t      axis_count;
};

/**
 * 驱动层那几个协议码在 `ncl_err_name()` 里是 "UnknownError"，这里补一句人话 ——
 * 现场排障时 `last_error` 就是这一句（例："写宏变量: 应答块返回码非 0"）。
 */
static const char *focas_err_text(ncl_err code)
{
    switch (code) {
    case NCL_FOCAS_ERR_RB_CODE:
        return "应答块返回码非 0（机床不收这条）";
    case NCL_FOCAS_ERR_NO_DATA:
        return "机床回「没有这一号」（方向 3）";
    case NCL_FOCAS_ERR_RB_COUNT:
        return "应答块数为 0";
    case NCL_FOCAS_ERR_RB_MISSING:
        return "应答少了一块（块数与请求对不上）";
    case NCL_FOCAS_ERR_LENGTH:
        return "应答长度不对";
    case NCL_FOCAS_ERR_MAGIC:
        return "帧头（magic）不对";
    case NCL_FOCAS_ERR_HEADER:
        return "帧头字段不对";
    default:
        return ncl_err_name(code);
    }
}

/**
 * 机床自己那套 `EW_xxx` 返回码的人话（传输三件套的状态回执里会带回来）。
 *
 * 表照 `Fwlib64.h` 抄的（2026-09-23 从官方头文件里逐条对出来）—— 之前这一格
 * 一直把 `5` 当 EW_ATTRIB，其实是 **EW_DATA（数据错）**；`4` 才是 EW_ATTRIB。
 * 现场表现就是 §11.18 那条"程序号已存在"被说成"属性不对"，白绕了一圈。
 */
static const char *focas_ew_text(int code)
{
    switch (code) {
    case 0: return "EW_OK";
    case 1: return "EW_FUNC（这个机型/状态没有这条功能）";
    case 2: return "EW_LENGTH（长度不对）";
    case 3: return "EW_NUMBER/EW_RANGE（号或范围不对）";
    case 4: return "EW_ATTRIB/EW_TYPE（属性/类型不对，机床不收这条）";
    case 5: return "EW_DATA（数据错）";
    case 6: return "EW_NOOPT（这个选件没开）";
    case 7: return "EW_PROT（写保护）";
    case 8: return "EW_OVRFLOW（内存不够放下）";
    case 9: return "EW_PARAM（参数不让写）";
    case 10: return "EW_BUFFER（缓冲区空，再要一次）";
    case 11: return "EW_PATH（路径不对）";
    case 12: return "EW_MODE（模式不对）";
    case 13: return "EW_REJECT（机床这个状态下不干这件事）";
    case 14: return "EW_DTSRVR（数据服务器）";
    case 15: return "EW_ALARM（机床报警）";
    case 16: return "EW_STOP（急停/停止中）";
    case 21: return "EW_RD_RSTFIN（读到程序尾了）";
    default: return "未知码";
    }
}

/**
 * 状态回执里的**细码**（体 `[4..6)` = `cnc_getdtailerr` 的 `ODBERR.err_no`）。
 *
 * 细码的含义**按帧分**（spec 的 `cnc_dwnstart4` / `cnc_dwnend4` / `cnc_upend4`
 * 各有一张表），这里把同号的几种含义并在一起给 —— 现场看的是"机床到底是嫌
 * 什么"，一个字都不给才是最难查的。
 */
static const char *focas_ew_detail_text(int code, int detail)
{
    if (code != 5) { /* 只有 EW_DATA 挂细码表 */
        return NULL;
    }
    switch (detail) {
    case 1: return "目录名不对（下行 start）/ 正文里有非法字符（下行 end）";
    case 2: return "TV check 下块里字符数是奇数（下行 end）/ 指定范围里没有程序（上行 end）";
    case 3: return "已登记的程序数满了（下行 end）/ 程序内存坏了（上行 end）";
    case 4: return "这个程序号已经登记过（下行 end）";
    case 5: return "这个程序号正被机床选中（下行 end）";
    default: return NULL;
    }
}

/** 记下这次失败（绑定的 NG 理由就是这一句）。 */
static ncl_err note(ncl_focas *focas, const char *what, ncl_err code)
{
    if (focas != NULL) {
        if (NCL_FOCAS_ERR_IS_TRANSFER(code)) {
            /* 程序上/下行的状态回执（应答方向 3，体 = 返回码 + 细码，§11.14）。 */
            int ew = NCL_FOCAS_TRANSFER_CODE(code);
            int detail = NCL_FOCAS_TRANSFER_DETAIL(code);
            const char *hint = focas_ew_detail_text(ew, detail);

            if (hint != NULL) {
                snprintf(focas->error, sizeof(focas->error),
                         "%s: 机床回码 %d（%s），细码 %d（%s）", what, ew,
                         focas_ew_text(ew), detail, hint);
            } else {
                snprintf(focas->error, sizeof(focas->error),
                         "%s: 机床回码 %d（%s）", what, ew, focas_ew_text(ew));
            }
        } else {
            snprintf(focas->error, sizeof(focas->error), "%s: %s", what,
                     focas_err_text(code));
        }
    }
    return code;
}

/*
 * 线上"每轴/每主轴一条记录"的形状（2026-09 真机实测，01 册 §2.8）：
 *
 *     [0..4)  data  (BE32)
 *     [4..6)  预留（这台机器恒 0x000a，官方库一个字段都不取）
 *     [6..8)  dec   (BE16，该轴的小数位)
 *
 * 值 = `data / 10^dec`。位置/伺服负载/主轴负载/主轴转速/进给速度**都是这一个形状**：
 * 载荷长度 256 字节 = 32 根轴 × 8（`0x26`/`0x56`）、64 字节 = 8 根主轴 × 8（`0x40`）。
 * dec 的位置是拿官方 SDK 的 `cnc_rdaxisdata`（cls=1）对同一台机器核的：它报 X 的
 * dec=3，而载荷里那一格正好在记录 **+6**（+4 处是恒定的 `00 0a`）。
 *
 * **原来按 12 字节 POSELM 切**（data@0 + dec@4 + 轴名@10，照假机床定的）：真机上
 * 会把 dec 读成 10，于是任何值都被除成 0（X 的 116583 成了 0 而不是 116.583），
 * 从第 2 根轴起偏移也全错。
 */
#define FOCAS_AXIS_RECORD 8u
#define FOCAS_AXIS_DEC_AT 6u

/** 加工件数 / 加工总件数在 0i 上的参数号（现场那份服务的 getPartCount/getPartTotal）。 */
#define FOCAS_PARAM_PART_COUNT 6711
#define FOCAS_PARAM_PART_TOTAL 6712
/** 参数记录的载荷长度：`0x8d` 读回来 264 字节、`0x8e` 写回去也是（§11.25）。 */
#define NCL_FOCAS_PARAM_RECORD 264u

/** `cnc_rdpdf_line` 那条载荷给的是**定长 256 字节**的程序路径（官方 SDK 就是这么发的）。 */
#define FOCAS_PDF_PATH_SIZE 256u

/** 一次读几行 / 整段程序最多读多少字节（防"要一个巨大的程序"吃爆内存）。 */
#define FOCAS_PDF_LINES_PER_CALL 64
#define FOCAS_PDF_PROGRAM_MAX (256u * 1024u)

/*
 * "同一个 item、每次问不同的号"那条读法（刀补/宏变量/参数/工件坐标都走它），
 * 定义在文件后半段 —— 这里先声明，前面的件数/程序目录也要用。
 */
static ncl_err record_call(ncl_focas *focas, const char *item, long long number,
                           long long arg2, const char *what, ncl_json **value);

/** 从"整块载荷的字节数组"里取一个字节（大端，与线上一致）。 */
static uint8_t bytes_at(const ncl_json *bytes, size_t index)
{
    long long value = 0;

    (void)ncl_json_as_int(ncl_json_arr_get((ncl_json *)bytes, index), &value);
    return (uint8_t)(value & 0xFF);
}

/** 取第 @p index 条记录（data + dec），载荷不够就是"读不到"。 */
static bool record_read(const ncl_json *payload, size_t index, int32_t *data,
                        int *dec)
{
    size_t at = index * FOCAS_AXIS_RECORD;

    if (payload == NULL || ncl_json_type_of((ncl_json *)payload) != NCL_JSON_ARRAY ||
        ncl_json_arr_len((ncl_json *)payload) < at + FOCAS_AXIS_RECORD) {
        return false;
    }
    *data = (int32_t)(((uint32_t)bytes_at(payload, at) << 24) |
                      ((uint32_t)bytes_at(payload, at + 1) << 16) |
                      ((uint32_t)bytes_at(payload, at + 2) << 8) |
                      (uint32_t)bytes_at(payload, at + 3));
    *dec = (int)(((uint16_t)bytes_at(payload, at + FOCAS_AXIS_DEC_AT) << 8) |
                 (uint16_t)bytes_at(payload, at + FOCAS_AXIS_DEC_AT + 1u));
    if (*dec < 0 || *dec > 9) {
        *dec = 0; /* 机床没报小数位（或报了怪值）：按整数算，别把值除成 0 */
    }
    return true;
}

/**
 * 这条记录是不是**空号**（spec 里 custom macro variable 的 "vacant"：`mcr_val = 0`
 * 且 `dec_val = -1`）。
 *
 * 为什么要单独判：`record_read()` 会把小数位夹到 0..9（-1 那种"没定义"的写法会变成
 * 0），拿夹过的 dec 判不出空号来。
 */
static bool record_is_vacant(const ncl_json *payload, size_t index)
{
    size_t at = index * FOCAS_AXIS_RECORD;

    if (payload == NULL ||
        ncl_json_type_of((ncl_json *)payload) != NCL_JSON_ARRAY ||
        ncl_json_arr_len((ncl_json *)payload) < at + FOCAS_AXIS_RECORD) {
        return false;
    }
    return bytes_at(payload, at) == 0 && bytes_at(payload, at + 1u) == 0 &&
           bytes_at(payload, at + 2u) == 0 && bytes_at(payload, at + 3u) == 0 &&
           bytes_at(payload, at + FOCAS_AXIS_DEC_AT) == 0xFF &&
           bytes_at(payload, at + FOCAS_AXIS_DEC_AT + 1u) == 0xFF;
}

/** 缩放到实际值：`data / 10^dec`（dec 是小数点位数）。 */
static double record_scale(int32_t data, int dec)
{
    double scale = 1.0;
    int i;

    for (i = 0; i < dec; i++) {
        scale *= 10.0;
    }
    return (double)data / scale;
}

/**
 * 这台机床实际有几根轴（`cnc_rdaxisname` 的轴名表：每轴 4 字节）。读一次就记住 ——
 * 机床的轴数不会变。读不到就回错，让调用方如实报"读不到"，别拿垃圾当坐标。
 */
static ncl_err axis_count(ncl_focas *focas, size_t *count)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    size_t length;
    ncl_err rc;

    if (focas->axis_count > 0) {
        *count = focas->axis_count;
        return NCL_OK;
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "AXISNAME");
    (void)ncl_json_obj_set_int(params, "block", 0);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, "AXISNAME", NCL_ERR_PARSE);
    }
    length = ncl_json_arr_len(bytes) / 4u; /* 每轴 4 字节 */
    ncl_json_free(answer);
    if (length == 0) {
        return note(focas, "AXISNAME", NCL_ERR_NOT_FOUND);
    }
    focas->axis_count = length;
    *count = length;
    return NCL_OK;
}

/** 这个轴号在机床上有吗（`which` 0 基）。没有就如实回"没有"。 */
static ncl_err axis_check(ncl_focas *focas, ncl_focas_axis axis, const char *what)
{
    size_t count = 0;
    ncl_err rc;

    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, what, NCL_ERR_RANGE);
    }
    rc = axis_count(focas, &count);
    if (rc != NCL_OK) {
        return rc;
    }
    if ((size_t)axis >= count) {
        return note(focas, what, NCL_ERR_NOT_FOUND); /* 这台机床没有这根轴 */
    }
    return NCL_OK;
}

/**
 * 读"每轴一条 8 字节记录"那一族里的第 @p index 条：item 名 + 块号 + 记录号。
 * 载荷里从哪开始写在 item 名字里（`"ACTF@8"`），块号是另一个参数 —— 与 §2.3 的读取
 * 约定一致（`"@offset"` 是载荷内的字节偏移）。
 *
 * 一次**只读这一条**（8 字节）而不是整块：有的调用只回它有的那几条 —— `cnc_acts`
 * 在只有一根主轴的机床上就回 8 字节，按"主轴数 × 8"去要会撞载荷不够。
 */
static ncl_err record_at(ncl_focas *focas, const char *item_name, long long block,
                         int index, double *value, int *dec_out)
{
    char item[48];
    ncl_json *payload = NULL;
    int32_t data = 0;
    int dec = 0;
    ncl_err rc;

    if (index < 0 || index > 63) {
        return note(focas, item_name, NCL_ERR_RANGE);
    }
    snprintf(item, sizeof(item), "%s@%d", item_name,
             index * (int)FOCAS_AXIS_RECORD);
    rc = ncl_focas_read_item(focas, item, block, (int)FOCAS_AXIS_RECORD,
                             NCL_DTYPE_BYTE, &payload);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!record_read(payload, 0, &data, &dec)) {
        ncl_json_free(payload);
        return note(focas, item_name, NCL_ERR_PARSE);
    }
    ncl_json_free(payload);
    *value = record_scale(data, dec);
    if (dec_out != NULL) {
        *dec_out = dec;
    }
    return NCL_OK;
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
 * 这一条 2026-09-23 **换了来源**：以前走 `cnc_rdcount`（item `RDCOUNT` = 一个 `0x8b`，
 * 载荷 @20；切法是官方 SDK 反查 + `libfwlib32.so` 反汇编两条对出来的，01 册 §2.6）——
 * 可那是**刀具寿命计数器**，机床要开"刀具寿命管理"选件才答；这台 0i-MF 上回
 * **EW_NOOPT=6**（如实报"机床不提供"，见 §11.13）。
 *
 * 加工件数在 0i 上是**第 6711 号参数**（加工总件数 6712）—— 现场那份服务的
 * `getPartCount` / `getPartTotal` 就是读这两号（`focas.cpp`）。参数这一路
 * （`cnc_rdparam` = 一个 `0x8d`，载荷 @0 的 BE32 是值）在这台机器上稳稳读得到，
 * 所以这里改读 6711：语义对（加工件数），而且真机/模拟器都不需要选件。
 */
ncl_err ncl_focas_part_count(ncl_focas *focas, long long *value)
{
    ncl_json *json = NULL;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = 0;
    rc = ncl_focas_parameter(focas, FOCAS_PARAM_PART_COUNT, &json);
    if (rc != NCL_OK) {
        return rc;
    }
    *value = ncl_json_obj_get_int(json, "raw", 0);
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
 * 每轴/每主轴一条记录的那两条（`cnc_actf` 进给速度 / `cnc_acts` 主轴转速）：
 * item 名 + 第几个。载荷就是上面那种 **8 字节记录**（data@0 + dec@6，§2.8）——
 * 原来按"每轴一个 float32"读（@index*4），真机上是把记录的第 4 个字节当 float 开头，
 * 从"第 2 根轴"起就全错。真机实测：`0x25` 回 `00 00 08 97 00 0a 00 00` → 2199 rpm。
 */
static ncl_err per_unit_record(ncl_focas *focas, const char *item_name,
                               int index, int count, double *value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0 || index >= count) {
        return note(focas, item_name, NCL_ERR_RANGE);
    }
    return record_at(focas, item_name, 0, index, value, NULL);
}

/* 轴的实际进给速度 F（cnc_actf，0x24）：每轴一条记录。 */
ncl_err ncl_focas_axis_feedrate(ncl_focas *focas, ncl_focas_axis axis,
                                double *value)
{
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = axis_check(focas, axis, "ACTF");
    if (rc != NCL_OK) {
        return rc;
    }
    return per_unit_record(focas, "ACTF", (int)axis, (int)NCL_FOCAS_AXIS_COUNT,
                           value);
}

/* 主轴的实际转速 S（cnc_acts，0x25）：每个主轴一条记录。 */
ncl_err ncl_focas_spindle_speed(ncl_focas *focas, unsigned spindle,
                                double *value)
{
    return per_unit_record(focas, "ACTS", (int)spindle,
                           (int)NCL_FOCAS_SPINDLE_MAX, value);
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

/* 当前刀具号（表 7 的 TOOL_NUMBER）：模态 T 码，cnc_rdgcode 一条里带 T/B/S/F。 */
ncl_err ncl_focas_tool_number(ncl_focas *focas, long long *value)
{
    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /*
     * **还没找到可靠的来源**，先如实回"读不到"：
     *
     *   - 模态那条 `cnc_rdgcode`（0x96）只报 G 组（24 组全试过，没有 T 那一格）；
     *   - `cnc_rdexecprog` 在这台机器上回的**不是"当前那一段"，而是整段程序文本**
     *     （真机实测 515 字节：M98P3001 … 一直到程序末尾），从里面"找最后一个 T"
     *     取到的会是程序里最后一个换刀，不是当前刀具 —— 那还不如不报。
     *
     * 等找到"当前 T 码"的正式出处（`cnc_rdgcode` 的别的 type，或 ODBDY2 里那一格）
     * 再接。模型里的 `/MACHINE/TOOL` 这一格先保持"读不到"，别编一个数。
     */
    /*
     * 当前刀号在**指令值**里：`cnc_rdcommand`（item `RDCOMMAND` = **0x97**，
     * `d = -1` 全读模态非 G 码）回一串 12 字节记录，一条一个地址：
     *
     *     [adrs(1)][num(1)][flag(2)][cmd_val(4 BE)][dec_val(4 BE)]
     *
     * 地址就是字母本身（`'T'` 0x54 = 刀号、`'M'`、`'S'`、`'F'`…），**找 `adrs == 'T'`
     * 那条的 `cmd_val` 就是当前刀号**（2026-09-23 官方 SDK 对 NCGuide 0i-MF 抓帧，
     * 一起抓到的还有 D/E/F/H/L/M/N/O/S/T 十条，见 01 册 §11.19）。
     *
     * 这里**没抓到 T 那条**（比如机床一把刀都没选）就如实回"读不到"，不报 0 ——
     * 0 是"选了 0 号刀"，与"没读到"是两回事。
     */
    {
        ncl_json *params = ncl_json_new_object();
        ncl_json *answer = NULL;
        ncl_json *bytes = NULL;
        size_t length;
        size_t at;
        bool found = false;
        long long tool = 0;
        ncl_err rc;

        if (params == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_string(params, "item", "RDCOMMAND");
        (void)ncl_json_obj_set_int(params, "block", 0);
        (void)ncl_json_obj_set_int(params, "d", -1); /* -1 = 全部模态非 G 码 */
        rc = ncl_focas_call(focas, "payload", params, &answer);
        ncl_json_free(params);
        if (rc != NCL_OK) {
            return rc;
        }
        bytes = ncl_json_obj_get(answer, "bytes");
        if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
            ncl_json_free(answer);
            return note(focas, "RDCOMMAND", NCL_ERR_PARSE);
        }
        length = ncl_json_arr_len(bytes);
        for (at = 0; at + 12u <= length; at += 12u) {
            if (bytes_at(bytes, at) == (uint8_t)'T') {
                tool = ((long long)bytes_at(bytes, at + 4u) << 24) |
                       ((long long)bytes_at(bytes, at + 5u) << 16) |
                       ((long long)bytes_at(bytes, at + 6u) << 8) |
                       (long long)bytes_at(bytes, at + 7u);
                found = true;
                break;
            }
        }
        ncl_json_free(answer);
        if (!found) {
            return note(focas, "当前刀具号", NCL_ERR_NOT_FOUND);
        }
        *value = tool;
    }
    return NCL_OK;
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

/*
 * 报警消息（`cnc_rdalmmsg2`，item `ALMMSG` = 一个 `0x23`：d = 报警类型（-1 = 全部）、
 * e = 最多几条、`arg2 = 2`（要文本）、`arg3 = 64`（文本要多少字节））。
 *
 * **没有报警时载荷 0 字节**（真机实测）→ 回空数组："没有报警"，不是"读不到"。
 * 有报警时载荷是**一条接一条的定长记录**（真机实测，§2.8.2）：
 *
 *     [0..4)   报警号    (BE32)   真机示例 75
 *     [4..8)   报警类型  (BE32)   3
 *     [8..12)  轴号/保留 (BE32)   0
 *     [12..16) 文本长度  (BE32)   4        ← GB2312 的"保护"正好 4 字节
 *     [16..)   文本（GB2312，文本长度那么多字节），后面补 0 到整条
 *
 * 整条长度 = `16 + arg3`（要 64 字节文本就是 80 字节一条）；`arg3` 给 0 时机床只回
 * 16 字节的抬头、**不填文本**，`arg2` 给 0 时同样不填 —— 这两格必须照上面给。
 */
#define FOCAS_ALM_HEADER 16u
#define FOCAS_ALM_TEXT_AT 16u
#define FOCAS_ALM_TEXT_MAX 64u
#define FOCAS_ALM_RECORD (FOCAS_ALM_HEADER + FOCAS_ALM_TEXT_MAX)

ncl_err ncl_focas_alarm(ncl_focas *focas, ncl_json **value)
{
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_json *array = NULL;
    size_t length = 0;
    size_t records;
    size_t i;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "ALMMSG");
    (void)ncl_json_obj_set_int(params, "block", 0);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, "ALMMSG", NCL_ERR_PARSE);
    }
    length = ncl_json_arr_len(bytes);
    array = ncl_json_new_array();
    if (array == NULL) {
        ncl_json_free(answer);
        return NCL_ERR_NOMEM;
    }
    records = length / FOCAS_ALM_RECORD;
    for (i = 0; i < records; i++) {
        size_t at = i * FOCAS_ALM_RECORD;
        long long number = 0;
        long long type = 0;
        size_t text_len = 0;
        char raw[FOCAS_ALM_TEXT_MAX + 1u];
        char *text = NULL;
        size_t used = 0;
        size_t j;
        ncl_json *entry;

        for (j = 0; j < 4u; j++) {
            number = (number << 8) | (long long)bytes_at(bytes, at + j);
            type = (type << 8) | (long long)bytes_at(bytes, at + 4u + j);
            text_len = (text_len << 8) | (size_t)bytes_at(bytes, at + 12u + j);
        }
        if (text_len > FOCAS_ALM_TEXT_MAX) {
            text_len = FOCAS_ALM_TEXT_MAX; /* 机床报的比我们要的多：按我们要的截 */
        }
        for (j = 0; j < text_len && at + FOCAS_ALM_TEXT_AT + j < length; j++) {
            unsigned byte = bytes_at(bytes, at + FOCAS_ALM_TEXT_AT + j);

            if (byte == 0) {
                break;
            }
            raw[used++] = (char)byte;
        }
        while (used > 0 && raw[used - 1] == ' ') {
            used--;
        }
        /* 机床给的是 GB2312，NC-Link 的 JSON 要 UTF-8（见 ncl_charset.h）。 */
        if (ncl_gb2312_to_utf8(raw, used, &text, NULL) != NCL_OK) {
            ncl_json_free(array);
            ncl_json_free(answer);
            return NCL_ERR_NOMEM;
        }
        entry = ncl_json_new_object();
        if (entry == NULL ||
            ncl_json_obj_set_int(entry, "number", number) != NCL_OK ||
            ncl_json_obj_set_int(entry, "type", type) != NCL_OK ||
            ncl_json_obj_set_string(entry, "text", text) != NCL_OK ||
            ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_free_safe(text);
            ncl_json_free(array);
            ncl_json_free(answer);
            return NCL_ERR_NOMEM;
        }
        ncl_free_safe(text);
    }
    ncl_json_free(answer);
    *value = array;
    return NCL_OK;
}

/** 整张刀具参数表（表 7 的 TOOLPARAM，JSON 对象）：**号 → 参数**。 */
ncl_err ncl_focas_tool_param_table(ncl_focas *focas, ncl_json **value)
{
    ncl_json *list = NULL;
    ncl_json *table = NULL;
    size_t i;
    size_t n;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    rc = ncl_focas_tool_list(focas, &list);
    if (rc != NCL_OK) {
        return rc;
    }
    table = ncl_json_new_object();
    if (table == NULL) {
        ncl_json_free(list);
        return NCL_ERR_NOMEM;
    }
    n = ncl_json_arr_len(list);
    for (i = 0; i < n; i++) {
        ncl_json *one = ncl_json_arr_get(list, i);
        char key[24];
        long long id = ncl_json_obj_get_int(one, "id", 0);

        snprintf(key, sizeof(key), "%lld", id);
        if (ncl_json_obj_set(table, key, ncl_json_clone(one)) != NCL_OK) {
            ncl_json_free(table);
            ncl_json_free(list);
            return NCL_ERR_NOMEM;
        }
    }
    ncl_json_free(list);
    *value = table;
    return NCL_OK;
}

/**
 * 坐标：`cnc_rdposition` 一条请求拿四路（绝对/机械/相对/剩余），请求 8 个块、应答
 * 8 个块一一对应（§2.3 的约定），所以 **下标 1 = 绝对、2 = 机械、3 = 相对、4 = 剩余**
 * （0 基），每块载荷就是上面那种"每轴 8 字节"的记录数组。
 *
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
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = axis_check(focas, axis, "RDPOSITION");
    if (rc != NCL_OK) {
        return rc; /* 这台机床没有这根轴，或者轴表都读不到 */
    }
    /* 块号从 1 起：1 = 绝对、2 = 机械、3 = 相对、4 = 剩余。 */
    return record_at(focas, "RDPOSITION", 1 + which, (int)axis, value, dec_out);
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
    rc = axis_check(focas, axis, "SV_DELAY");
    if (rc != NCL_OK) {
        return rc;
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
    *value = record_scale(data, dec);
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

/*
 * 伺服负载（`cnc_rdsvmeter`）：item `SVMETER` = 一个 `0x56`（d=1 = 全部轴），
 * 应答 256 字节，每轴 8 字节（形状同位置：data@0、dec@6，§2.8）。机床静止时是 0。
 */
ncl_err ncl_focas_axis_load(ncl_focas *focas, ncl_focas_axis axis,
                            double *value)
{
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = axis_check(focas, axis, "SVLOAD");
    if (rc != NCL_OK) {
        return rc;
    }
    return record_at(focas, "SVMETER", 0, (int)axis, value, NULL);
}

/*
 * 主轴负载（`cnc_rdspmeter`，item `SPLOAD` = 一个 `0x40`，d=4）：应答 64 字节 =
 * 8 根主轴 × 8（同一种 8 字节记录）。主轴号是**主轴**编号，不是轴号。
 */
ncl_err ncl_focas_spindle_load(ncl_focas *focas, unsigned spindle,
                               double *value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (spindle >= NCL_FOCAS_SPINDLE_MAX) {
        return note(focas, "SPLOAD", NCL_ERR_RANGE);
    }
    return record_at(focas, "SPLOAD", 0, (int)spindle, value, NULL);
}

/**
 * 轴扭矩 / 负载扭矩（`cnc_loadtorq`，item `TORQUE` = 一个 **0xfd**：d = 电机号
 * （**0 = 伺服电机**）、e = 轴号（**1 起**：X = 1）、应答载荷 4 字节）。
 *
 * 2026-09 对 NCGuide 0i-MF Plus 实测：`d=0 e=1` → 块返回码 0、载荷 `00000000`；
 * `d=3 e=7` → 回 **EW_RANGE=4**（这台只有 3 根轴、电机号 0/1）—— 所以"机床收这条、
 * 且按轴号校验"是核过的。**但值这一格没法定标**：这台机器静止，回答恒 0；载荷 4
 * 字节又是一个整值，看不出是"负载率%"还是别的量纲。所以这里按"载荷 @0 的 BE32"
 * 取，**量纲留给站点在 get_attributes 里注明**，不假装它是百分比。
 */
ncl_err ncl_focas_axis_torque(ncl_focas *focas, ncl_focas_axis axis,
                              double *value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    uint32_t raw;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "AXIS", NCL_ERR_RANGE);
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "TORQUE");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", 0);            /* 0 = 伺服电机 */
    (void)ncl_json_obj_set_int(params, "e", (int)axis + 1); /* 轴号从 1 起 */
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < 4) {
        ncl_json_free(answer);
        return note(focas, "轴扭矩", NCL_ERR_NOT_FOUND);
    }
    raw = ((uint32_t)bytes_at(bytes, 0) << 24) | ((uint32_t)bytes_at(bytes, 1) << 16) |
          ((uint32_t)bytes_at(bytes, 2) << 8) | (uint32_t)bytes_at(bytes, 3);
    ncl_json_free(answer);
    *value = (double)(int32_t)raw;
    return NCL_OK;
}

/*
 * 轴电流（安培）：还是 `cnc_rdsvmeter`（**0x56**），只是 `d` 换一格 ——
 * 官方 SDK 的 `cnc_rdaxisdata(cls = 2 Servo, type = 1/2)` 发的就是这条，
 * `d = 1` 是**负载表（%）**、`d = 3` 是**负载电流（A）**（2026-09-23 抓帧，
 * 01 册 §11.19：`cls=2 type=1` → `0x56 d=1`、`type=2` → `0x56 d=3`）。
 * 记录形状与负载那一族一样：`data@0` + `dec@6`。
 *
 * **轴温不用做了**：FOCAS 里**没有**读轴温的调用（官方头 Fwlib64.h + spec 全文搜过，
 * 只有"智能终端高温报警"那种报警码），所以那个点位列直接删掉，不留"待抓包"。
 */
ncl_err ncl_focas_axis_current(ncl_focas *focas, ncl_focas_axis axis,
                               double *value)
{
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = axis_check(focas, axis, "SVCURRENT");
    if (rc != NCL_OK) {
        return rc;
    }
    return record_at(focas, "SVCURRENT", 0, (int)axis, value, NULL);
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
    {
        unsigned char name = 0;
        ncl_json *params = ncl_json_new_object();
        ncl_json *answer = NULL;
        ncl_json *bytes = NULL;
        size_t length;
        ncl_err rc;

        if (params == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_string(params, "item", "AXISNAME");
        (void)ncl_json_obj_set_int(params, "block", 0);
        rc = ncl_focas_call(focas, "payload", params, &answer);
        ncl_json_free(params);
        if (rc != NCL_OK) {
            return rc;
        }
        bytes = ncl_json_obj_get(answer, "bytes");
        if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
            ncl_json_free(answer);
            return note(focas, "AXIS", NCL_ERR_PARSE);
        }
        length = ncl_json_arr_len(bytes);
        if ((size_t)axis * 4u + 4u > length) {
            ncl_json_free(answer);
            return note(focas, "AXIS", NCL_ERR_NOT_FOUND);
        }
        name = bytes_at(bytes, (size_t)axis * 4u);
        ncl_json_free(answer);
        switch (name) {
    case 'X': case 'x': case 'Y': case 'y': case 'Z': case 'z':
    case 'U': case 'u': case 'V': case 'v': case 'W': case 'w':
        snprintf(out, cap, "%s", "linear");
        return NCL_OK;
    case 'A': case 'a': case 'B': case 'b': case 'C': case 'c':
        snprintf(out, cap, "%s", "rotary");
        return NCL_OK;
    default:
        return note(focas, "AXIS", NCL_ERR_NOT_FOUND);
        }
    }
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
    /*
     * 这台机器**没有**单独的"F 值"那条可用（`cnc_rddynamic2` 死活回 rc=4，长度从 4
     * 试到 192 都一样），但每轴的进给速度 `cnc_actf`（item `ACTF`）是通的 —— 官方
     * SDK 的 `cnc_rdaxisdata(cls=5 速度)` 最后也就是发 `0x24`×N（真机抄包确认）。
     * 所以按"三根直线轴里最大的那个"给：机床只有一个方向在动时就是它。
     */
    {
        static const ncl_focas_axis kAxes[] = { NCL_FOCAS_AXIS_X, NCL_FOCAS_AXIS_Y,
                                                NCL_FOCAS_AXIS_Z };
        double best = 0.0;
        size_t ok = 0;
        size_t i;
        ncl_err rc = NCL_OK;
        ncl_err last = NCL_OK;

        for (i = 0; i < sizeof(kAxes) / sizeof(kAxes[0]); i++) {
            double one = 0.0;

            rc = ncl_focas_axis_feedrate(focas, kAxes[i], &one);
            if (rc != NCL_OK) {
                /* 这台机器 `0x24` 只回**一根轴**的 8 字节（真机实测）：
                 * 第 2、3 根读不到不算错，跳过；三根都读不到才把错报上去。 */
                last = rc;
                continue;
            }
            ok++;
            if (one > best) {
                best = one;
            }
        }
        if (ok == 0) {
            return last;
        }
        *value = best;
        return NCL_OK;
    }
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
 *
 * **注意机床可能根本没填这一块**：真机（0i-MD 仿真）上 `0x5d` 的载荷 32 字节里除
 * `@2 = 0xffff` 全是 0 —— 于是读出来是 0%（现场看到"倍率一直 0"先查这个，别改偏移）。
 * 偏移本身是按 IODBSGNL 的结构定的（01 册 §1/§2.8.7）。
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
 * 主轴倍率：操作面板信号 `IODBSGNL.spdl_ovrd` —— 就在**进给倍率后面那一格**
 * （item 表里那张偏移图：`… rpd_ovrd@6、jog_ovrd@8、feed_ovrd@0xa、spdl_ovrd@0xc`），
 * 还是 `cnc_rdopnlsgnl`（**0x5d**）。码值→百分比与进给倍率同一张表（0..20 = 0%..200%，
 * 每级 10%）。
 *
 * 两点如实说清楚：
 *  1. spec 的 `slct_data` 位上写着 **bit6 = 主轴倍率信号"只有 Series 15i"**，而 0i-D
 *     那版头文件 `Fwlib64.h` 的 `IODBSGNL` **有** `spdl_ovrd` 这一格（15i 那版才标
 *     "(not used)"）—— 所以字段存在、能不能填要看机型。这里按结构体偏移读，
 *     读不到就如实报（不编一个 100%）。
 *  2. **本机（NCGuide 0i-MF）没填这一块**：`0x5d` 回的载荷是没初始化的内容。
 *     落进 0..20 码表时就"看着像 0%"（现场看到倍率一直是 0 先查这里），落在码表外
 *     就如实报 `NCL_ERR_RANGE` —— 两种都是"这块是空的"，**换真机要复核**。
 */
ncl_err ncl_focas_spindle_override(ncl_focas *focas, double *value)
{
    ncl_json *json = NULL;
    long long code = 0;
    ncl_err rc;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDSGNL@12", 0, 1, NCL_DTYPE_INT16, &json);
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

/* 正在执行的程序段（cnc_rdexecprog）：应答里是"程序行文本"。 */
/*
 * 正在执行的程序段（`cnc_rdexecprog`，item `EXECPROG` = 一个 `0x20`，d = 要多少字节）：
 * 应答体 = 4 字节 + ASCII 文本（NUL/0 补齐）。真机实测那条：
 *
 *     M98P3001 \n\n G49 \n\n T01 \n D1 \n G0G43H1Z100. \n M…
 *
 * 出门就是这一整段文本（换行照原样，行尾补 0 的不算）。
 */
ncl_err ncl_focas_executed_block(ncl_focas *focas, char *out, size_t cap)
{
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    size_t length;
    size_t i;
    ncl_err rc;

    if (focas == NULL || out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "EXECPROG");
    (void)ncl_json_obj_set_int(params, "block", 0);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, "EXECPROG", NCL_ERR_PARSE);
    }
    length = ncl_json_arr_len(bytes);
    /* 文本从 @4 起（前 4 字节官方库也不读）；机床用 0 补齐，遇到第一个 0 就是尾。 */
    {
        size_t used = 0;

        for (i = 4u; i < length && used + 1u < cap; i++) {
            unsigned byte = bytes_at(bytes, i);

            if (byte == 0) {
                break;
            }
            out[used++] = (char)byte;
        }
        out[used] = '\0';
    }
    ncl_json_free(answer);
    /* 收尾：把尾部的 0 与空白去掉（机床是补 0 的）。 */
    {
        size_t len = strlen(out);

        while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\n' ||
                           out[len - 1] == '\r')) {
            out[--len] = '\0';
        }
    }
    return NCL_OK;
}

/* 程序目录（cnc_rdprogdir3，0x06，d = 0x13）：PRGDIR3 数组，形状见 31 册 §1 #7。 */
/*
 * 程序目录（`cnc_rdprogdir3`，item `RDPROGDIR3` = 一个 `0x06`：d=0、e=8、arg2=1）：
 * 应答是 **72 字节一条**的记录，一条一个程序（真机实测，§2.8.4）：
 *
 *     [0..2)   （空）
 *     [2..4)   程序号  (BE16)   例：2001
 *     [4..8)   4 字节类型/属性（第一个程序是 "M 0 "，其余这台是 0）
 *     [8..72)  注释（NUL 结尾，后面补 0）
 *
 * 出门是一个数组：`[{"number":2001,"comment":"(DEMOMAINGEAR)"}, …]`。
 */
#define FOCAS_PROG_RECORD 72u
#define FOCAS_PROG_NUMBER_AT 2u
#define FOCAS_PROG_COMMENT_AT 8u

/** 一次问机床要几条（item 表里 `RDPROGDIR3` 的 `e`）。 */
#define FOCAS_PROG_DIR_PAGE 8
/** 最多翻多少页：`d` = 起始程序号，机床要是不认它就会原地打转，兜个底。 */
#define FOCAS_PROG_DIR_PAGES 64

/**
 * 读一页程序目录（`d` = 从哪个程序号开始），记录追加进 @p array，
 * 回这一页**收下**的条数与最大程序号。
 *
 * @p floor 是"上一页已经拿到的最大号"：比它小的记录一律不收 —— 有的机床不认
 * `d`（每页都把同一批号重铺一遍），照收就会把同一个程序列两遍。
 */
static ncl_err program_dir_page(ncl_focas *focas, long long start,
                                long long floor, ncl_json *array,
                                size_t *records, long long *last)
{
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    size_t length;
    size_t count;
    size_t i;
    ncl_err rc;

    *records = 0;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDPROGDIR3");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", start); /* 起始程序号（翻页靠它） */
    (void)ncl_json_obj_set_int(params, "e", FOCAS_PROG_DIR_PAGE);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, "RDPROGDIR3", NCL_ERR_PARSE);
    }
    length = ncl_json_arr_len(bytes);
    count = length / FOCAS_PROG_RECORD;
    for (i = 0; i < count; i++) {
        size_t at = i * FOCAS_PROG_RECORD;
        long long number = ((long long)bytes_at(bytes, at + FOCAS_PROG_NUMBER_AT)
                            << 8) |
                           (long long)bytes_at(bytes, at + FOCAS_PROG_NUMBER_AT + 1u);
        char comment[FOCAS_PROG_RECORD];
        size_t used = 0;
        size_t j;
        ncl_json *entry;

        for (j = FOCAS_PROG_COMMENT_AT; at + j < length && j < FOCAS_PROG_RECORD;
             j++) {
            unsigned byte = bytes_at(bytes, at + j);

            if (byte == 0) {
                break;
            }
            comment[used++] = (char)byte;
        }
        while (used > 0 && comment[used - 1] == ' ') {
            used--;
        }
        comment[used] = '\0';
        if (number == 0 && used == 0) {
            continue; /* 空槽（机床把没占的格子也铺出来了） */
        }
        if (number <= floor) {
            continue; /* 上一页已经收过的号 */
        }
        entry = ncl_json_new_object();
        if (entry == NULL ||
            ncl_json_obj_set_int(entry, "number", number) != NCL_OK ||
            ncl_json_obj_set_string(entry, "comment", comment) != NCL_OK ||
            ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(answer);
            return NCL_ERR_NOMEM;
        }
        if (number > *last) {
            *last = number;
        }
        (*records)++;
    }
    ncl_json_free(answer);
    return NCL_OK;
}

/*
 * 翻页：一次只回 8 条，得顺着程序号问下去。
 *
 * 2026-09-23 在这台 NCGuide 0i-MF 上核的（01 册 §11.16.1）：库里当时有 12 个程序，
 * 原来只发一条请求，目录就**少了一半**（回的是号最小的那八个）—— 现场"程序列表
 * 不全"就是这么来的。`d` = 起始程序号，下一轮从这一页最大号 +1 接着问，问到空为止。
 */
ncl_err ncl_focas_program_directory(ncl_focas *focas, ncl_json **value)
{
    ncl_json *array = NULL;
    long long start = 0;
    size_t page;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (page = 0; page < FOCAS_PROG_DIR_PAGES; page++) {
        long long last = 0;
        long long floor = start > 0 ? start - 1 : 0; /* 上一页收过的最大号 */
        size_t records = 0;
        ncl_err rc = program_dir_page(focas, start, floor, array, &records, &last);

        if (rc != NCL_OK) {
            if (page == 0) {
                ncl_json_free(array);
                return rc;
            }
            break; /* 后面几页出岔子：把已经拿到的交出去，别整张丢掉 */
        }
        if (records == 0) {
            break; /* 这一页一条新的都没有 = 到头了（收录时已经滤掉旧号） */
        }
        if (last + 1 <= start) {
            break; /* 防御：号没有往前推进，再问也是原地打转 */
        }
        start = last + 1;
    }
    *value = array;
    return NCL_OK;
}

/*
 * 刀补类型（`cnc_rdtofs` / `cnc_wrtofs` 的 `type`）：线上它落在 Cb 的 `arg2`，而且
 * 是 **arg2 = 1000 + type** —— 2026-09 用官方 SDK 对 NCGuide 0i-MF Plus 抓帧实测
 * （`cnc_rdtofs 1 1` → Cb 0x08、d=1、e=1、**a2=1001**；type=0 时 a2=1000，
 * 01 册 §11.13）。四个字段的对应关系是拿两边老代码对出来的，两边一致：
 * 官方头 `FWLIB32.H` 的 `cnc_rdtofs(h, short ofs_num, short type, short len,
 * ODBTOFS *)`，现场那份服务 `NCLINK-SERVICE/mods/focas/focas.cpp` 的
 * `getToolParams`（type 1 → radius、3 → length、0 → radius_abrasion、
 * 2 → length_abrasion，值都是 `data / 1000.0`）。
 */
#define FOCAS_TOFS_RADIUS      1
#define FOCAS_TOFS_RADIUS_WEAR 0
#define FOCAS_TOFS_LENGTH      3
#define FOCAS_TOFS_LENGTH_WEAR 2

/** 刀补值的"一个最低输入单位"（本机 dec = 3 → 0.001mm；读不到 dec 时按这个算）。 */
#define FOCAS_TOFS_DEC 3

/** 整张刀具表最多读多少号（见 ncl_focas_tool_list 的说明）。 */
#define FOCAS_TOOL_TABLE_MAX 64

static double tofs_scale(int dec)
{
    double scale = 1.0;
    int i;

    for (i = 0; i < dec; i++) {
        scale *= 10.0;
    }
    return scale;
}

/**
 * 刀补号的上限（`cnc_rdtofsinfo` = 一个 `0x0a`，应答载荷 8 字节，
 * `ODBTLINF = {short ofs_type; short use_no}`）。本机回 `0000 0190 …` → **400**。
 * 刀具列表就靠它定"读到第几号"（`cnc_rdtooldata` 这台机器回 EW_FUNC=1，不给）。
 */
ncl_err ncl_focas_tool_offset_count(ncl_focas *focas, long long *count)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    long long n;
    ncl_err rc;

    if (focas == NULL || count == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *count = 0;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDTOFSINFO");
    (void)ncl_json_obj_set_int(params, "block", 0);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < 4) {
        ncl_json_free(answer);
        return note(focas, "刀补表信息", NCL_ERR_PARSE);
    }
    n = ((long long)bytes_at(bytes, 2) << 8) | (long long)bytes_at(bytes, 3);
    ncl_json_free(answer);
    if (n <= 0) {
        return note(focas, "刀补表信息", NCL_ERR_NOT_FOUND);
    }
    *count = n;
    return NCL_OK;
}

/**
 * 一套刀具参数（表 7 的 TOOLPARAM）：现场口径是"**一把刀就是一个元素**"，形状对齐
 * 册 4 的 `id/kind/radius/length`（与 10 册新代那条同口径）：
 *
 *   `{"id":1,"kind":0,"radius":0,"length":0,"radius_wear":0,"length_wear":0}`
 *
 * `kind`（刀尖号）这条路上**没有来源**：官方 SDK 的 `cnc_rdtofs` 四个 type 里没有
 * 刀尖号，`cnc_rdtooldata` 这台机器回 EW_FUNC=1 不给。所以 `kind` 固定 0，要真刀尖
 * 号的站在点位表里自己覆盖（册 4 原文也注着"需要再确认"）。
 * `time_usage`（寿命）同样不给：`cnc_rdlife` 这台机器回 **EW_NOOPT=6**（刀具寿命
 * 管理选项没开），不编数。
 */
static ncl_err tool_param_read(ncl_focas *focas, long long index, ncl_json **out)
{
    static const struct {
        const char *field;
        int         type;
    } kFields[] = {{"radius", FOCAS_TOFS_RADIUS},
                   {"length", FOCAS_TOFS_LENGTH},
                   {"radius_wear", FOCAS_TOFS_RADIUS_WEAR},
                   {"length_wear", FOCAS_TOFS_LENGTH_WEAR}};
    ncl_json *object;
    size_t i;
    ncl_err first = NCL_OK;

    object = ncl_json_new_object();
    if (object == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_int(object, "id", index);
    (void)ncl_json_obj_set_int(object, "kind", 0); /* 见上面：没有来源，固定 0 */
    for (i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++) {
        ncl_json *one = NULL;
        ncl_err rc = record_call(focas, "RDTOFS", index,
                                 1000 + kFields[i].type, "RDTOFS", &one);

        if (rc == NCL_OK) {
            (void)ncl_json_obj_set_double(object, kFields[i].field,
                                          ncl_json_obj_get_double(one, "value",
                                                                  0.0));
        } else {
            (void)ncl_json_obj_set_double(object, kFields[i].field, 0.0);
            if (first == NCL_OK) {
                first = rc; /* 第一处失败留着当"这条为什么没值" */
            }
        }
        ncl_json_free(one);
    }
    /*
     * 四个字段一个都读不到 = 这台没有这一号（空号回空载荷 → NOT_FOUND）。
     * 不半真半假地给一张全 0 的表。
     */
    if (first == NCL_ERR_NOT_FOUND) {
        ncl_json_free(object);
        return note(focas, "刀具参数", NCL_ERR_NOT_FOUND);
    }
    *out = object;
    return NCL_OK;
}

ncl_err ncl_focas_tool_param(ncl_focas *focas, long long index,
                             ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0) {
        return note(focas, "刀具参数", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return tool_param_read(focas, index, value);
}

/**
 * 整张刀具表（表 7 的 TOOL，list）：**逐号读**（每个号 4 条 0x08），从 1 号读到
 * `cnc_rdtofsinfo` 给的 `use_no`，最多 `FOCAS_TOOL_TABLE_MAX` 号。这台机器
 * `use_no = 400`，400 × 4 条报文对"配置读"太重，而且真机上大部分号是空的
 * （空号回空载荷 → 跳过，不进表）。上限是本实现的约定，要更多就改这个宏。
 */
ncl_err ncl_focas_tool_list(ncl_focas *focas, ncl_json **value)
{
    ncl_json *array = NULL;
    long long limit = 0;
    long long index;
    long long found = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    rc = ncl_focas_tool_offset_count(focas, &limit);
    if (rc != NCL_OK) {
        return rc; /* 连"有几个号"都问不到，就别装能列出来 */
    }
    if (limit > FOCAS_TOOL_TABLE_MAX) {
        limit = FOCAS_TOOL_TABLE_MAX;
    }
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (index = 1; index <= limit; index++) {
        ncl_json *one = NULL;

        rc = tool_param_read(focas, index, &one);
        if (rc == NCL_ERR_NOT_FOUND) {
            continue; /* 空号：不进表 */
        }
        if (rc != NCL_OK) {
            ncl_json_free(array);
            return rc;
        }
        if (ncl_json_arr_push(array, one) != NCL_OK) {
            ncl_json_free(one);
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
        found++;
    }
    /*
     * 一个号都没读到（机床不给刀补表）时如实回"读不到"，不交一张空表 ——
     * 空表会被上层的门面当成"这台机床没有刀"。
     */
    if (found == 0) {
        ncl_json_free(array);
        return note(focas, "刀具列表", NCL_ERR_NOT_FOUND);
    }
    *value = array;
    return NCL_OK;
}

/*
 * 一条刀补（`cnc_rdtofs`，item `RDTOFS` = 一个 `0x08`：d = 刀补号、e = 1、
 * `arg2 = 1000`）。应答就是那条 **8 字节记录**（data@0、dec@6，§2.8.2 同族形状），
 * 真机实测刀补 1 回 `00 00 00 00 00 0a 00 03` → 0.000 mm。
 *
 * 官方 SDK 的 `cnc_rdtofs` 出参也是"先把载荷头 4 字节当值"（斜坡载荷反查过：
 * 出参的 data 格跟着载荷 @0 走），所以这一条的字段就是"值 + 小数位"两格。
 */
static ncl_err record_call(ncl_focas *focas, const char *item, long long number,
                           long long arg2, const char *what, ncl_json **value)
{
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    int32_t data = 0;
    int dec = 0;
    ncl_err rc;

    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", item);
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    /*
     * `e` 也给**同一个号**：官方 SDK 对这台机器单条读时 `d` 与 `e` 是同一个数
     * （`cnc_rdparam 6711` → `0x8d d=6711 e=6711`；`cnc_wrtofs 2 …` → `0x09 d=2 e=2`，
     * 2026-09 抓帧实测，01 册 §11.13）。以前这里写死 1，于是**只有 1 号读得出来**：
     * 参数 6711、刀补 2 都回块返回码非 0。单条读/写时 d 与 e 本来就该相等，
     * 所以这一格就是"号"。
     */
    (void)ncl_json_obj_set_int(params, "e", number);
    if (arg2 >= 0) {
        (void)ncl_json_obj_set_int(params, "arg2", arg2);
    }
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        /* 机床回方向 3 = "没有这个号"（见 ncl_focas_pdu.h 里那个码）。 */
        return note(focas, what, NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, what, NCL_ERR_PARSE);
    }
    if (ncl_json_arr_len(bytes) < (long long)FOCAS_AXIS_RECORD) {
        /*
         * 机床回了空载荷 = "没有这个号"（真机实测：宏变量 100 / 刀补 2 / 参数 2
         * 都是这样，rc=0 但载荷 0 字节，块码还是 0x00）。这不是"读错了"，别报成
         * 解析失败 —— 上层按"这台没有那一号"处理更对。
         */
        ncl_json_free(answer);
        return note(focas, what, NCL_ERR_NOT_FOUND);
    }
    if (!record_read(bytes, 0, &data, &dec)) {
        ncl_json_free(answer);
        return note(focas, what, NCL_ERR_PARSE);
    }
    ncl_json_free(answer);
    *value = ncl_json_new_object();
    if (*value == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_int(*value, "number", number);
    (void)ncl_json_obj_set_double(*value, "value", record_scale(data, dec));
    /* 原始整数也带上：写那一侧要"写后复核"（比原始值，不跟小数位/量纲纠缠）。 */
    (void)ncl_json_obj_set_int(*value, "raw", (long long)data);
    return NCL_OK;
}

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
    /*
     * 不带类型的那一条按 **type 0**（半径磨损）走：`arg2 = 1000 + 0` —— 这是
     * 2026-09 真机上核过的形状，与 `ncl_focas_tool_param()` 的四个字段各对一格。
     */
    return record_call(focas, "RDTOFS", index, 1000 + FOCAS_TOFS_RADIUS_WEAR,
                       "RDTOFS", value);
}

/** 带类型的一条刀补（type 0..3，见上面那张表）。 */
ncl_err ncl_focas_tool_offset_typed(ncl_focas *focas, long long index,
                                    long long type, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0 || type < 0 || type > 3) {
        return note(focas, "RDTOFS", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    return record_call(focas, "RDTOFS", index, 1000 + type, "RDTOFS", value);
}

/**
 * 数一段文本里有几**整行**—— `cnc_rdpdf_line` 每次只回"文字"，回了多少行得自己数，
 * 才知道下一段从第几行接着读。
 *
 * 按 spec 的口径：**最后一行没有读到 EOB（'\n'）就不算一行**（"読込まれた最後の行が
 * その行の終わりEOB('\n')まで読込まれていない場合、読込んだ行数としてはカウントされ
 * ません"）。所以这里只数 '\n'，而"末尾不是 '\n'"正好当**程序读完**的标志用（见下面
 * `ncl_focas_program_upload` 的循环）。
 */
static long long program_count_lines(const char *text, size_t len)
{
    long long lines = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        if (text[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

/**
 * 按文件名按行读一段程序（`cnc_rdpdf_line`，item `RDPDFLINE` = Cb **`0xf0`**）。
 *
 * 帧（2026-09-23 抓的，01 册 §11.16）：
 *
 *     Cb       code 0xf0、`d` = 起始行号（程序头 = 0）、`e` = 读几行、
 *              **`tag1` = 载荷长度**、块长 = `0x1c + 载荷`
 *     载荷     程序路径（**256 字节，NUL 补齐**；`//CNC_MEM/USER/PATH1/O3001`）
 *     应答     体就是**程序正文**（本机对 O3001 回了 384 字节）
 *
 * 路径必须是"盘名 + 路径 + 文件名"：给裸文件名（`O3001`）机床回 `EW_DATA`（细码 1 =
 * 程序路径错）。
 */
static ncl_err program_read_lines(ncl_focas *focas, const char *path,
                                  long long start_line, long long lines,
                                  ncl_json **value)
{
    ncl_json *params;
    ncl_json *data;
    ncl_json *answer = NULL;
    ncl_json *bytes;
    ncl_json *out;
    char padded[FOCAS_PDF_PATH_SIZE];
    size_t i;
    ncl_err rc;

    memset(padded, 0, sizeof(padded));
    snprintf(padded, sizeof(padded), "%s", path);
    params = ncl_json_new_object();
    data = ncl_json_new_array();
    if (params == NULL || data == NULL) {
        ncl_json_free(params);
        ncl_json_free(data);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < sizeof(padded); i++) {
        if (ncl_json_arr_push(data, ncl_json_new_int((unsigned char)padded[i])) !=
            NCL_OK) {
            ncl_json_free(params);
            ncl_json_free(data);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_obj_set_string(params, "item", "RDPDFLINE");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", start_line);
    (void)ncl_json_obj_set_int(params, "e", lines);
    (void)ncl_json_obj_set(params, "data", data);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return note(focas, "RDPDFLINE", rc);
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, "RDPDFLINE", NCL_ERR_PARSE);
    }
    out = ncl_json_new_object();
    if (out == NULL) {
        ncl_json_free(answer);
        return NCL_ERR_NOMEM;
    }
    {
        size_t n = ncl_json_arr_len(bytes);
        char *text = (char *)ncl_mem_calloc(n + 1u, 1u);

        if (text == NULL) {
            ncl_json_free(answer);
            ncl_json_free(out);
            return NCL_ERR_NOMEM;
        }
        for (i = 0; i < n; i++) {
            text[i] = (char)bytes_at(bytes, i);
        }
        (void)ncl_json_obj_set_string(out, "path", path);
        (void)ncl_json_obj_set_int(out, "line", start_line);
        (void)ncl_json_obj_set_int(out, "lines",
                                   program_count_lines(text, n));
        (void)ncl_json_obj_set_string(out, "text", text);
        ncl_mem_free(text);
    }
    ncl_json_free(answer);
    *value = out;
    return NCL_OK;
}

/**
 * 读一条刀补的**原始记录**（值 + 小数位）。写之前要靠它拿机床自己的 dec：
 * 值是按"最低输入单位"送的，dec 不对值就差 10 倍。
 */
static ncl_err tofs_read_raw(ncl_focas *focas, long long index, int type,
                             int32_t *data, int *dec)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_err rc;

    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDTOFS");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", index);
    (void)ncl_json_obj_set_int(params, "e", index); /* d = e = 号（见 record_call） */
    (void)ncl_json_obj_set_int(params, "arg2", 1000 + type);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        return note(focas, "RDTOFS", NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < (long long)FOCAS_AXIS_RECORD) {
        ncl_json_free(answer);
        return note(focas, "RDTOFS", NCL_ERR_NOT_FOUND); /* 空载荷 = 没有这一号 */
    }
    if (!record_read(bytes, 0, data, dec)) {
        ncl_json_free(answer);
        return note(focas, "RDTOFS", NCL_ERR_PARSE);
    }
    ncl_json_free(answer);
    return NCL_OK;
}

/**
 * 写一条刀补（`cnc_wrtofs` = item `WRTOFS` = **0x09**）。
 *
 * 帧是 2026-09 用官方 SDK 对 NCGuide 0i-MF Plus 抓下来、再用本 client 写进去核过的
 * （01 册 §11.13）：
 *
 *     Cb      0x09、d = 刀补号、e = 1、arg2 = 1000 + type、块长度格 = 0x1c + 8
 *     载荷    8 字节 = BE32 值 + BE16 0 + BE16 0xffff      （tag0/tag1 保持 0）
 *
 * 值按机床自己的小数位换算（**先读一次打底拿 dec**，读不到就按 0.001mm）——
 * 写 0x3333 到 1 号、再读回来是 `00003333 000a 0003` = 13.107mm，一模一样。
 *
 * 权限不在这里判（现场口径：**权限在适配器外面控**），这里只提供能力。
 */
static ncl_err tofs_write(ncl_focas *focas, long long index, int type,
                          double value)
{
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    uint8_t payload[8];
    int32_t raw;
    int dec = FOCAS_TOFS_DEC;
    size_t i;
    ncl_err rc;

    /* 先读一次原始记录：既能拿机床自己的 dec，又把空号挡在写之前。 */
    rc = tofs_read_raw(focas, index, type, &raw, &dec);
    if (rc != NCL_OK) {
        return rc;
    }
    raw = (int32_t)(value * tofs_scale(dec) + (value < 0.0 ? -0.5 : 0.5));
    payload[0] = (uint8_t)((uint32_t)raw >> 24);
    payload[1] = (uint8_t)((uint32_t)raw >> 16);
    payload[2] = (uint8_t)((uint32_t)raw >> 8);
    payload[3] = (uint8_t)(uint32_t)raw;
    payload[4] = 0x00;
    payload[5] = 0x00;
    payload[6] = 0xFF; /* 机床自己的小数位说了算：这一格写 0xffff */
    payload[7] = 0xFF;

    params = ncl_json_new_object();
    bytes = ncl_json_new_array();
    if (params == NULL || bytes == NULL) {
        ncl_json_free(params);
        ncl_json_free(bytes);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < sizeof(payload); i++) {
        if (ncl_json_arr_push(bytes, ncl_json_new_int(payload[i])) != NCL_OK) {
            ncl_json_free(params);
            ncl_json_free(bytes);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_obj_set_string(params, "item", "WRTOFS");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", index);
    (void)ncl_json_obj_set_int(params, "e", index); /* d = e = 号（官方 SDK 同形） */
    (void)ncl_json_obj_set_int(params, "arg2", 1000 + type);
    (void)ncl_json_obj_set(params, "data", bytes);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "写刀补", rc);
    }
    return NCL_OK;
}

ncl_err ncl_focas_tool_offset_write_typed(ncl_focas *focas, long long index,
                                          long long type, double value)
{
    if (focas == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0 || type < 0 || type > 3) {
        return note(focas, "写刀补", NCL_ERR_INVALID_ARG);
    }
    if (value > 2147483647.0 || value < -2147483648.0) {
        return note(focas, "写刀补", NCL_ERR_RANGE);
    }
    return tofs_write(focas, index, (int)type, value);
}

/**
 * 写一套刀具参数（表 7 的 TOOLPARAM）：**只写给出的那几个字段**，其余不动
 * （每个字段一条 0x09，与 `ncl_focas_tool_param()` 的字段一一对应）。
 */
ncl_err ncl_focas_tool_param_write(ncl_focas *focas, long long index,
                                   const ncl_json *fields)
{
    static const struct {
        const char *field;
        int         type;
    } kFields[] = {{"radius", FOCAS_TOFS_RADIUS},
                   {"length", FOCAS_TOFS_LENGTH},
                   {"radius_wear", FOCAS_TOFS_RADIUS_WEAR},
                   {"length_wear", FOCAS_TOFS_LENGTH_WEAR}};
    size_t i;
    size_t written = 0;

    if (focas == NULL || fields == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (index < 0 || ncl_json_type_of((ncl_json *)fields) != NCL_JSON_OBJECT) {
        return note(focas, "写刀具参数", NCL_ERR_INVALID_ARG);
    }
    for (i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++) {
        if (!ncl_json_obj_has(fields, kFields[i].field)) {
            continue;
        }
        {
            ncl_err rc = tofs_write(focas, index, kFields[i].type,
                                    ncl_json_obj_get_double(fields,
                                                            kFields[i].field,
                                                            0.0));

            if (rc != NCL_OK) {
                return rc;
            }
        }
        written++;
    }
    if (written == 0) {
        return note(focas, "写刀具参数", NCL_ERR_INVALID_ARG); /* 没有可写的字段 */
    }
    return NCL_OK;
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

/*
 * 一个用户宏变量（`cnc_rdmacro`，item `RDMACRO` = 一个 `0x15`：d = 变量号、e = 1）。
 * 应答 8 字节：data@0（BE32）、dec@6 —— 真机实测 `00 00 00 00 00 0a ff ff` → 值 0；
 * dec 那格这台机器给 0xffff（不是 0..9 的合法小数位，按 0 算）。
 */
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
    return record_call(focas, "RDMACRO", number, -1, "RDMACRO", value);
}
/*
 * 读一条"参数记录"：**264 字节**，机床 `0x8d` 应答的原样（形状 2026-09-23 在这台
 * NCGuide 0i-MF 上逐字节核出来的，01 册 §11.26）：
 *
 *     @0..4    号（BE32，机床把问的号回一遍）
 *     @4..6    轴号（BE16）
 *     @6..8    属性 prm_type（BE16：bit0..1 = 类型 0=bit/1=byte/2=word/3=2字，
 *              bit2 = 带轴，bit5 = 写保护…… 见 spec `cnc_rdparainfo`）
 *     @8..12   **值**（BE32，有符号）
 *     @12..    其余：写的时候照抄回去，机床认这一份形状
 *
 * ⚠️ 以前这里按 "@4 = 条数、@8 = 值" 读。`@4` 那 4 字节其实是 **轴号 + 属性** 两格，
 * 对无轴参数（轴 0、属性 3）恰好看着"对"；带轴参数（1320/1420/1825…）照老样子读
 * 机床直接回"块返回码非 0"。轴号在请求里是 `arg2` —— 官方 `cnc_rdparam(h, number,
 * axis, length, IODBPSD*)` 的 `length` 根本不上线，上线的是 axis。
 */
static ncl_err parameter_record(ncl_focas *focas, long long number, long long axis,
                                uint8_t *record)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    long long length;
    ncl_err rc;
    size_t i;

    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDPARAM");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number); /* d = e = 号（官方 SDK 同形） */
    (void)ncl_json_obj_set_int(params, "arg2", axis);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        return note(focas, "RDPARAM", NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    length = bytes != NULL && ncl_json_type_of(bytes) == NCL_JSON_ARRAY
                 ? ncl_json_arr_len(bytes)
                 : 0;
    if (length < 12) {
        ncl_json_free(answer);
        return note(focas, "RDPARAM", NCL_ERR_NOT_FOUND); /* 空载荷 = 没有这一号 */
    }
    if (record != NULL) {
        /*
         * 机床一般给满 264 字节；给少了（比如单测里的假机床）就补 0 —— 写的时候
         * 机床只认前 12 字节那几格，尾巴补 0 它也收（§11.26 实测）。
         */
        memset(record, 0, NCL_FOCAS_PARAM_RECORD);
        for (i = 0; i < NCL_FOCAS_PARAM_RECORD && (long long)i < length; i++) {
            record[i] = (uint8_t)bytes_at(bytes, i);
        }
    }
    ncl_json_free(answer);
    return NCL_OK;
}

static ncl_err parameter_read(ncl_focas *focas, long long number, long long axis,
                              ncl_json **value)
{
    uint8_t record[NCL_FOCAS_PARAM_RECORD];
    int32_t data;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (number < 0 || axis < 0) {
        return note(focas, "RDPARAM", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    rc = parameter_record(focas, number, axis, record);
    if (rc != NCL_OK) {
        return rc;
    }
    data = (int32_t)(((uint32_t)record[8] << 24) | ((uint32_t)record[9] << 16) |
                     ((uint32_t)record[10] << 8) | (uint32_t)record[11]);
    *value = ncl_json_new_object();
    if (*value == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (ncl_json_obj_set_int(*value, "number", number) != NCL_OK ||
        ncl_json_obj_set_int(*value, "axis", axis) != NCL_OK ||
        ncl_json_obj_set_int(*value, "type",
                             ((long long)record[6] << 8) | (long long)record[7]) !=
            NCL_OK ||
        ncl_json_obj_set_double(*value, "value", (double)data) != NCL_OK ||
        ncl_json_obj_set_int(*value, "raw", (long long)data) != NCL_OK) {
        ncl_json_free(*value);
        *value = NULL;
        return NCL_ERR_NOMEM;
    }
    return NCL_OK;
}

/** 一个 CNC 参数（无轴那条；带轴的用 `ncl_focas_parameter_axis`）。 */
ncl_err ncl_focas_parameter(ncl_focas *focas, long long number, ncl_json **value)
{
    return parameter_read(focas, number, 0, value);
}

/** 一个带轴 CNC 参数：@p axis = 1..n（0 只对无轴参数有效）。 */
ncl_err ncl_focas_parameter_axis(ncl_focas *focas, long long number, long long axis,
                                 ncl_json **value)
{
    return parameter_read(focas, number, axis, value);
}

/* ------------------------------------------------------------------- PMC -- */

/*
 * PMC（FANUC 的 PLC 就叫 PMC）：`pmc_rdpmcrng`（item `PMCRNG` = **0x8001**）。
 * 帧是 2026-09-23 用官方 SDK 对 NCGuide 0i-MF 抓的（01 册 §11.21）：
 *
 *     请求载荷 = [起始号 BE32][结束号 BE32][族 BE32][宽度 BE32]（+ 数据区）
 *     族（adr_type）：0=G 1=F 2=Y 3=X 4=A 5=R 6=T 7=K 8=C 9=D
 *     宽度（data_type）：0=字节 1=字 2=长字；**结束号 = 起始号 + 要几个**
 *     应答载荷 = 数据（每点 1/2/4 字节，大端）
 *
 * `pmc_rdpmcinfo`（**0x8003**）能问出各族机床实际支持的号段，但那 772 字节载荷的
 * 逐条布局还没核（字母每 12 字节一条，见 §11.21.4）—— 所以 `get_length` 这轮先用
 * spec 上按系列列的范围（0i-D 那一版），不是问机床要的。
 */

/** 族字母 → `pmc_rdpmcrng` 的 `adr_type`；认不出回 -1。 */
int ncl_focas_pmc_adr_type(char family)
{
    switch (family) {
    case 'G': case 'g': return 0;
    case 'F': case 'f': return 1;
    case 'Y': case 'y': return 2;
    case 'X': case 'x': return 3;
    case 'A': case 'a': return 4;
    case 'R': case 'r': return 5;
    case 'T': case 't': return 6;
    case 'K': case 'k': return 7;
    case 'C': case 'c': return 8;
    case 'D': case 'd': return 9;
    default: return -1;
    }
}

/**
 * 读一段 PMC 号（@p count 个点，从 @p start 起，单位是该族自己的单位：X/Y/R 是**字节**、
 * D 是**字**）。@p width：0 字节 / 1 字（D 用这个）。
 */
ncl_err ncl_focas_pmc_read(ncl_focas *focas, char family, long long start,
                           long long count, int width, ncl_json **value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_json *array = NULL;
    int adr = ncl_focas_pmc_adr_type(family);
    size_t point = width == 2 ? 4u : (width == 1 ? 2u : 1u);
    size_t need;
    size_t i;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    if (adr < 0 || start < 0 || count <= 0 || width < 0 || width > 2) {
        return note(focas, "PMC", NCL_ERR_INVALID_ARG);
    }
    if (count > 512) {
        return note(focas, "PMC", NCL_ERR_RANGE); /* 一段别要太多：应答体放不下 */
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "PMCRNG");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", start);
    (void)ncl_json_obj_set_int(params, "e", start + count); /* 结束号 = 起始 + 个数 */
    (void)ncl_json_obj_set_int(params, "arg2", adr);
    (void)ncl_json_obj_set_int(params, "arg3", width);
    /* 块头第 2 格：PMC 这一族官方库发 2（别的都是 1）—— 这一格不对机床回的块头是乱的 */
    (void)ncl_json_obj_set_int(params, "first", 2);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY) {
        ncl_json_free(answer);
        return note(focas, "PMC", NCL_ERR_PARSE);
    }
    need = (size_t)count * point;
    if ((size_t)ncl_json_arr_len(bytes) < need) {
        /*
         * 机床只回了**一个点**（本机 NCGuide 就是这样：一段要 8 个也只回 1 个）——
         * 退化成**一个号一个号读**把它补齐。这样"段读"在真机上更快、在只肯一个个
         * 回的机器上也照样能用。
         */
        size_t got = (size_t)ncl_json_arr_len(bytes) / point;

        ncl_json_free(answer);
        if (got == 0 || count > 64) {
            return note(focas, "PMC", NCL_ERR_RANGE);
        }
        array = ncl_json_new_array();
        if (array == NULL) {
            return NCL_ERR_NOMEM;
        }
        for (i = 0; i < (size_t)count; i++) {
            ncl_json *one = NULL;
            long long v = 0;
            ncl_json *entry;

            rc = ncl_focas_pmc_read(focas, family, start + (long long)i, 1, width,
                                    &one);
            if (rc != NCL_OK) {
                ncl_json_free(array);
                return rc;
            }
            (void)ncl_json_as_int(ncl_json_arr_get(one, 0), &v);
            ncl_json_free(one);
            entry = ncl_json_new_int(v);
            if (entry == NULL || ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(array);
                return NCL_ERR_NOMEM;
            }
        }
        *value = array;
        return NCL_OK;
    }
    array = ncl_json_new_array();
    if (array == NULL) {
        ncl_json_free(answer);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < (size_t)count; i++) {
        size_t at = i * point;
        long long v = 0;
        size_t j;
        ncl_json *entry;

        for (j = 0; j < point; j++) {
            v = (v << 8) | bytes_at(bytes, at + j);
        }
        entry = ncl_json_new_int(v);
        if (entry == NULL || ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(array);
            ncl_json_free(answer);
            return NCL_ERR_NOMEM;
        }
    }
    ncl_json_free(answer);
    *value = array;
    return NCL_OK;
}

/**
 * 写一段 PMC 号（`pmc_wrpmcrng` = item `PMCWR` = **0x8002**）。
 *
 * 帧（2026-09-23 抓的，01 册 §11.22）：与读同一个四格载荷，末尾再跟
 * `[数据长度 BE16][数据…]` —— 也就是驱动那句 `"data"`（这里给 `[len][值…]`）。
 * 一次写 `count` 个点（单位与读一致：位族按字节、D 按字）。
 *
 * **不是所有族都能写**（spec 明说 `F`、`X` 有些区不能写；`X`/`F` 本身是机床/CNC
 * 驱动的信号）：本实现照发，机床不收就如实把返回码带上来（`NCL_FOCAS_ERR_RB_CODE`
 * 或 `NCL_ERR_UNAVAILABLE`），**绝不假装成功**。
 */
ncl_err ncl_focas_pmc_write(ncl_focas *focas, char family, long long start,
                            const long long *values, size_t count, int width)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *data = NULL;
    int adr = ncl_focas_pmc_adr_type(family);
    size_t point = width == 2 ? 4u : (width == 1 ? 2u : 1u);
    size_t i;
    ncl_err rc;

    if (focas == NULL || values == NULL || count == 0u) {
        return NCL_ERR_INVALID_ARG;
    }
    if (adr < 0 || start < 0 || width < 0 || width > 2 || count > 128) {
        return note(focas, "PMC", NCL_ERR_INVALID_ARG);
    }
    params = ncl_json_new_object();
    data = ncl_json_new_array();
    if (params == NULL || data == NULL) {
        ncl_json_free(params);
        ncl_json_free(data);
        return NCL_ERR_NOMEM;
    }
    /*
     * 载荷 = **每个点 point 字节，大端**（大端，不带长度前缀）。
     *
     * 长度那一格落在**块头末尾那两格**（`[24..26)` = 0、`[26..28)` = 载荷长度）——
     * 驱动写载荷时会把它填上。为什么是这里：官方库写那帧的块尾是
     * `[00000002][AA 55]`（4 字节长度 + 2 字节数据，块长 30 = 8 + 16 + 6），
     * 也就是**长度紧跟在四格载荷后面**、正好压在块头那两格上（`[24..28)` 读成 BE32
     * 就是 2）。我们早先把长度又塞进载荷里，等于多送了 4 个字节
     * （2026-09-23 定位，01 册 §11.22.1）。
     */
    for (i = 0; i < count; i++) {
        uint64_t v = (uint64_t)values[i];
        size_t j;

        for (j = 0; j < point; j++) {
            size_t shift = 8u * (point - 1u - j);

            (void)ncl_json_arr_push(data,
                                    ncl_json_new_int((long long)((v >> shift) & 0xFF)));
        }
    }
    (void)ncl_json_obj_set_string(params, "item", "PMCWR");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", start);
    /*
     * **写这一侧 `e` 给 "起始 + 1"**（官方库抓到的帧就是 `s=0 e=1`，而载荷里带了
     * 2 个字节）—— 写几个由**载荷里的长度**说了算，`e` 这一格在这台机器上按 1 个
     * 地址就认（01 册 §11.22 实测：写 R0..R1 = 0x33 0x44 → 读回 R1 = 0x44）。
     * 真机上 `e` 要不要跟着个数走，抓一次再定。
     */
    (void)ncl_json_obj_set_int(params, "e", start + 1);
    (void)ncl_json_obj_set_int(params, "arg2", adr);
    (void)ncl_json_obj_set_int(params, "arg3", width);
    (void)ncl_json_obj_set_int(params, "first", 2);
    (void)ncl_json_obj_set(params, "data", data);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "写 PMC", rc);
    }
    /*
     * **写后复核**（与写刀具坐标/宏变量同一个口径）：读回来比一遍，对不上就如实回
     * "没写进去"（`NCL_ERR_UNAVAILABLE`），**绝不回成功**。
     *
     * 为什么非比不可：PMC 有些位是**梯形图在驱动**的 —— 回 rc=0 而值被当场改回去是
     * 常态（01 册 §11.22 实测：写 R0 = 0x33、读回 0x32）。读几遍、每遍隔 20 ms，
     * 给机床/梯形图一个扫描周期。
     */
    {
        unsigned attempt;

        for (attempt = 0; attempt < 3u; attempt++) {
            ncl_json *back = NULL;
            size_t j;
            bool same = true;

            rc = ncl_focas_pmc_read(focas, family, start, (long long)count, width,
                                    &back);
            if (rc != NCL_OK) {
                return rc;
            }
            for (j = 0; j < count && same; j++) {
                long long got = -1;

                (void)ncl_json_as_int(ncl_json_arr_get(back, j), &got);
                if (got != values[j]) {
                    same = false;
                }
            }
            ncl_json_free(back);
            if (same) {
                return NCL_OK;
            }
            if (attempt + 1u < 3u) {
                ncl_sleep_millis(20u);
            }
        }
        /*
         * 复核没过：**这台机床一次只落一个点**（与读那一侧同一个脾气，§11.21.3）——
         * 退化成**一个号一个号写**再来一遍；还是一个都落不下才如实报"没写进去"。
         */
        if (count > 1u) {
            for (i = 0; i < count; i++) {
                rc = ncl_focas_pmc_write(focas, family, start + (long long)i,
                                         &values[i], 1u, width);
                if (rc != NCL_OK) {
                    return rc;
                }
            }
            return NCL_OK;
        }
        return note(focas, "写 PMC", NCL_ERR_UNAVAILABLE);
    }
}

/**
 * 写一个 PMC **位**（位号 = 字节 × 8 + 位）：读回所在字节、改这一位、再写回去。
 * 这样调用方不用关心字节里别的位。
 */
ncl_err ncl_focas_pmc_bit_write(ncl_focas *focas, char family, long long bit,
                                bool on)
{
    ncl_json *one = NULL;
    long long byte_value = 0;
    long long out = 0;
    size_t shift;
    ncl_err rc;

    if (focas == NULL || bit < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_pmc_read(focas, family, bit / 8, 1, 0, &one);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_as_int(ncl_json_arr_get(one, 0), &byte_value);
    ncl_json_free(one);
    shift = (size_t)(bit % 8);
    if (on) {
        out = byte_value | (1LL << shift);
    } else {
        out = byte_value & ~(1LL << shift);
    }
    if (out == byte_value) {
        return NCL_OK; /* 已经是这个值：不用写 */
    }
    return ncl_focas_pmc_write(focas, family, bit / 8, &out, 1, 0);
}

/**
 * 问机床"这个族到底支持哪些号段"：`pmc_rdpmcinfo`（item `PMCINF` = **0x8003**，
 * `d = -1` 全族）。载荷 = `[记录数 BE32]` + 64 × `[族字母 2 字节][属性 2 字节]`
 * `[起始号 4][结束号 4]`（**大端**；2026-09-23 在这台 0i-MF 上核出来：G 一族 10 段、
 * `0..767` / `1000..1767` / … / `9000..9767`，与 spec 0i-D 那张表对得上）。
 *
 * 出门：`{"G":{"blocks":[[0,767],…],"count":<各段之和>}, …}`。
 */
ncl_err ncl_focas_pmc_info(ncl_focas *focas, ncl_json **value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_json *table = NULL;
    size_t length;
    size_t records;
    size_t i;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "PMCINF");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", -1);
    (void)ncl_json_obj_set_int(params, "first", 2);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < 4) {
        ncl_json_free(answer);
        return note(focas, "PMC", NCL_ERR_PARSE);
    }
    length = (size_t)ncl_json_arr_len(bytes);
    records = ((size_t)bytes_at(bytes, 0) << 24) | ((size_t)bytes_at(bytes, 1) << 16) |
              ((size_t)bytes_at(bytes, 2) << 8) | (size_t)bytes_at(bytes, 3);
    if (records > 64u || 4u + records * 12u > length) {
        ncl_json_free(answer);
        return note(focas, "PMC", NCL_FOCAS_ERR_LENGTH);
    }
    table = ncl_json_new_object();
    if (table == NULL) {
        ncl_json_free(answer);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < records; i++) {
        size_t at = 4u + i * 12u;
        char letter[2];
        long long top;
        long long last;
        ncl_json *family;
        ncl_json *blocks;
        ncl_json *block;

        letter[0] = (char)bytes_at(bytes, at + 1u); /* 字母在 2 字节格的低字节 */
        letter[1] = 0;
        if (letter[0] == '\0') {
            continue;
        }
        top = ((long long)bytes_at(bytes, at + 4u) << 24) |
              ((long long)bytes_at(bytes, at + 5u) << 16) |
              ((long long)bytes_at(bytes, at + 6u) << 8) |
              (long long)bytes_at(bytes, at + 7u);
        last = ((long long)bytes_at(bytes, at + 8u) << 24) |
               ((long long)bytes_at(bytes, at + 9u) << 16) |
               ((long long)bytes_at(bytes, at + 10u) << 8) |
               (long long)bytes_at(bytes, at + 11u);
        family = ncl_json_obj_get(table, letter);
        if (family == NULL || ncl_json_type_of(family) != NCL_JSON_OBJECT) {
            family = ncl_json_new_object();
            blocks = ncl_json_new_array();
            if (family == NULL || blocks == NULL ||
                ncl_json_obj_set(family, "blocks", blocks) != NCL_OK ||
                ncl_json_obj_set(table, letter, family) != NCL_OK) {
                ncl_json_free(family);
                ncl_json_free(table);
                ncl_json_free(answer);
                return NCL_ERR_NOMEM;
            }
            (void)ncl_json_obj_set_int(family, "count", 0);
        }
        blocks = (ncl_json *)ncl_json_obj_get(family, "blocks");
        block = ncl_json_new_array();
        if (block == NULL ||
            ncl_json_arr_push(block, ncl_json_new_int(top)) != NCL_OK ||
            ncl_json_arr_push(block, ncl_json_new_int(last)) != NCL_OK ||
            ncl_json_arr_push(blocks, block) != NCL_OK) {
            ncl_json_free(block);
            ncl_json_free(table);
            ncl_json_free(answer);
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(
            family, "count",
            ncl_json_obj_get_int(family, "count", 0) + (last - top + 1));
    }
    ncl_json_free(answer);
    *value = table;
    return NCL_OK;
}

/**
 * PMC 参数区的**控制数据**（数据表 `D` 或扩展继电器）：`pmc_rdcntldata` = **0x8004**、
 * `pmc_rdcntlexrelay` = **0x8057**（2026-09-23 抓帧，01 册 §11.24.2）。
 *
 * @p exrelay：false = 数据表 D（0x8004）、true = 扩展继电器（0x8057）。
 * **组号从 1 起**（给 0 机床回 EW_NUMBER）—— 所以这里从 1 号开始读，读到机床不收为止，
 * 出门 `{"1":{"size":10000,"address":0}, …}`（应答 8 字节 = `[表参数|数据类型 2]`
 * `[大小 2][地址 2][保留 2]`）。
 */
ncl_err ncl_focas_pmc_control_table(ncl_focas *focas, bool exrelay,
                                    ncl_json **value)
{
    ncl_json *table = NULL;
    long long group;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    table = ncl_json_new_object();
    if (table == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (group = 1; group <= 16; group++) { /* 这台机器 1 组；多读几组兜着 */
        ncl_json *params;
        ncl_json *answer = NULL;
        ncl_json *bytes = NULL;
        char key[24];
        ncl_json *entry;
        ncl_err rc;

        params = ncl_json_new_object();
        if (params == NULL) {
            ncl_json_free(table);
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_string(params, "item",
                                      exrelay ? "PMCEXRLY" : "PMCCNTL");
        (void)ncl_json_obj_set_int(params, "block", 0);
        (void)ncl_json_obj_set_int(params, "d", group);
        (void)ncl_json_obj_set_int(params, "e", group);
        (void)ncl_json_obj_set_int(params, "first", 2);
        rc = ncl_focas_call(focas, "payload", params, &answer);
        ncl_json_free(params);
        if (rc != NCL_OK) {
            break; /* 这个组号机床不收：到头了（或这台不支持这一族）*/
        }
        bytes = ncl_json_obj_get(answer, "bytes");
        if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
            ncl_json_arr_len(bytes) < 8) {
            ncl_json_free(answer);
            break;
        }
        entry = ncl_json_new_object();
        if (entry == NULL) {
            ncl_json_free(answer);
            ncl_json_free(table);
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(
            entry, "tableParam",
            ((long long)bytes_at(bytes, 0) << 8) | bytes_at(bytes, 1));
        (void)ncl_json_obj_set_int(
            entry, "size",
            ((long long)bytes_at(bytes, 2) << 8) | bytes_at(bytes, 3));
        (void)ncl_json_obj_set_int(
            entry, "address",
            ((long long)bytes_at(bytes, 4) << 8) | bytes_at(bytes, 5));
        snprintf(key, sizeof(key), "%lld", group);
        if (ncl_json_obj_set(table, key, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(answer);
            ncl_json_free(table);
            return NCL_ERR_NOMEM;
        }
        ncl_json_free(answer);
    }
    if (ncl_json_obj_len(table) == 0) {
        ncl_json_free(table);
        return note(focas, "PMC 控制数据", NCL_ERR_UNAVAILABLE);
    }
    *value = table;
    return NCL_OK;
}

/**
 * **PMC 自己的报警**（`pmc_rdalmmsg` = **0x8010**）：与 `cnc_alarm`（CNC 侧）不是一回事。
 * 载荷 = `[type = 1][起始报警号]`；**起始号给 0 机床回 EW_DATA**，所以这里默认从 1 号起。
 * 应答载荷 = `[报警号 4 字节]` + 文本（这台没有 PMC 报警 → 号 0 或全 `f` 的哨兵）。
 *
 * ⚠️ 文本的切法**没在真机上核过**（这台机器没有 PMC 报警）：这里按"号 4 字节 + 后面
 * 全是文本（NUL/0 截断、去尾空白）"出门；有真机报警时再对一次。
 */
ncl_err ncl_focas_pmc_alarm(ncl_focas *focas, long long start, long long count,
                            ncl_json **value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_json *array = NULL;
    long long number;
    char text[512];
    size_t i;
    size_t used = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (start < 1) {
        start = 1; /* 0 号机床回 EW_DATA，从 1 起 */
    }
    if (count <= 0 || count > 8) {
        count = 8;
    }
    *value = NULL;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "PMCALM");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", 1); /* type = 1（枚举出来的）*/
    (void)ncl_json_obj_set_int(params, "e", start);
    (void)ncl_json_obj_set_int(params, "first", 2);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc != NCL_OK) {
        return note(focas, "PMC 报警", rc);
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < 4) {
        ncl_json_free(answer);
        return note(focas, "PMC 报警", NCL_ERR_PARSE);
    }
    number = ((long long)bytes_at(bytes, 0) << 24) |
             ((long long)bytes_at(bytes, 1) << 16) |
             ((long long)bytes_at(bytes, 2) << 8) | (long long)bytes_at(bytes, 3);
    /* 没有报警：号 0 或全 1 的哨兵 → 回空数组（不是"读不到"）*/
    for (i = 0; i < (size_t)ncl_json_arr_len(bytes); i++) {
        if (bytes_at(bytes, i) != 0xFF) {
            break;
        }
    }
    if (number == 0) {
        ncl_json_free(answer);
        *value = ncl_json_new_array();
        return *value != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    for (i = 4; i < (size_t)ncl_json_arr_len(bytes) && used + 1u < sizeof(text);
         i++) {
        unsigned byte = bytes_at(bytes, i);

        if (byte == 0) {
            break;
        }
        text[used++] = (char)byte;
    }
    while (used > 0 && (text[used - 1] == ' ' || text[used - 1] == '\n')) {
        used--;
    }
    text[used] = '\0';
    ncl_json_free(answer);
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    {
        ncl_json *entry = ncl_json_new_object();

        if (entry == NULL ||
            ncl_json_obj_set_int(entry, "number", number) != NCL_OK ||
            ncl_json_obj_set_string(entry, "text", text) != NCL_OK ||
            ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
    }
    *value = array;
    return NCL_OK;
}

/**
 * 读一个 PMC **位**（I/O 那几族的梯形图地址就是"字节.位"：`X0.0` = 字节 0 的第 0 位）。
 * @p bit 是**扁平位号**（`字节 × 8 + 位`），与模型那侧 `/CONTROLLER/REGISTER@X` 的号一致。
 */
ncl_err ncl_focas_pmc_bit(ncl_focas *focas, char family, long long bit, bool *on)
{
    ncl_json *one = NULL;
    long long byte_value = 0;
    ncl_err rc;

    if (focas == NULL || on == NULL || bit < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    *on = false;
    rc = ncl_focas_pmc_read(focas, family, bit / 8, 1, 0, &one);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!ncl_json_as_int(ncl_json_arr_get(one, 0), &byte_value)) {
        ncl_json_free(one);
        return note(focas, "PMC", NCL_ERR_PARSE);
    }
    ncl_json_free(one);
    *on = ((byte_value >> (bit % 8)) & 1) != 0;
    return NCL_OK;
}

/*
 * 工件零点偏移（工件坐标系 G54…）—— `cnc_rdzofs`（item `RDZOFS` = **0x0b**）。
 *
 * 帧是 2026-09-23 用官方 SDK 对 NCGuide 0i-MF 抓的：请求四格是
 * `d` = 偏移号、`e` = 同一个号、`arg2` = 轴号（**-1 = ALL_AXES**）、`arg3` = 0；
 * 应答载荷 = **32 条 8 字节记录**（`MAX_AXIS` = 32），第 i 根轴的值在 `@8×(i-1)`，
 * 记录形状与位置/负载那一族**完全一样**（`data@0` + `dec@6`）。
 *
 *     number：0 = 外部零点偏移、1..6 = G54..G59、7..306 = G54.1P1..
 *
 * 写是 `cnc_wrzofs`（码按"读 + 1"推 = **0x0c**，实测机床认）：载荷与写刀补/宏变量
 * 一个形状（`[值 BE32][00 00][ff ff]`），但**一次只写一根轴**（`arg2` = 轴号，1 起；
 * 轴号给 -1 机床不收）。所以"写一个坐标系"= 逐轴写。
 */

/** "外部/G54/G54.1P3" 这种名字 → 偏移号；认不出回 -1。 */
static long long zofs_number(const char *name)
{
    const char *p = name;
    long long value = 0;

    while (*p == ' ') {
        p++;
    }
    if (ncl_streq_ignore_case(p, "EXT") || ncl_streq_ignore_case(p, "EXTERNAL") ||
        ncl_streq_ignore_case(p, "外部")) {
        return 0;
    }
    if (p[0] != 'G' && p[0] != 'g') {
        return -1;
    }
    p++;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    if (value < 54 || value > 59) {
        return -1;
    }
    if (*p == '\0') {
        return value - 54 + 1; /* G54 → 1 … G59 → 6 */
    }
    /*
     * `G54.1P<n>`：**取最后那段数字**当 P 号（"G54.1P3" 里的 `.1` 不是 P 号）。
     * 号 = 7 + n - 1。
     */
    {
        const char *last = NULL;
        const char *q;
        long long n = 0;

        for (q = p; *q != '\0'; q++) {
            if (*q == '.' || *q == 'p' || *q == 'P') {
                last = q + 1;
            }
        }
        if (last == NULL) {
            return -1; /* "G54x" 这种认不出的写法 */
        }
        for (q = last; *q >= '0' && *q <= '9'; q++) {
            n = n * 10 + (*q - '0');
        }
        if (n >= 1 && n <= 300 && *q == '\0') {
            return 7 + n - 1;
        }
    }
    return -1;
}

/** 把"名字"反过来：号 → `"G54"` / `"EXT"` / `"G54.1P3"`（给整表用）。 */
static void zofs_name(long long number, char *out, size_t cap)
{
    if (number == 0) {
        snprintf(out, cap, "EXT");
    } else if (number >= 1 && number <= 6) {
        snprintf(out, cap, "G%lld", 54 + number - 1);
    } else {
        snprintf(out, cap, "G54.1P%lld", number - 7 + 1);
    }
}

/** 读一个号的**全轴**载荷（32 条 8 字节记录）；调用方负责 ncl_json_free。 */
static ncl_err zofs_read(ncl_focas *focas, long long number, ncl_json **records)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_err rc;

    *records = NULL;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDZOFS");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number);
    (void)ncl_json_obj_set_int(params, "arg2", -1); /* ALL_AXES */
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        return note(focas, "工件坐标系", NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < (long long)FOCAS_AXIS_RECORD) {
        ncl_json_free(answer);
        return note(focas, "工件坐标系", NCL_ERR_NOT_FOUND);
    }
    *records = ncl_json_clone(bytes);
    ncl_json_free(answer);
    return *records != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/**
 * 一列轴记录 → `{"x":…,"y":…,"z":…}`。
 *
 * 键用**机床自己报的轴名**（`cnc_rdaxisname` 那张表，每轴 4 字节）：车床没有 Y，
 * 按顺序硬排会把 Z 写成 "y"。轴名表读不到时才退回按 X/Y/Z/A/C 排。
 */
static ncl_err zofs_axes_json(ncl_focas *focas, const ncl_json *records,
                              ncl_json **value)
{
    static const char *const kFallback[] = { "x", "y", "z", "a", "c" };
    char names[NCL_FOCAS_AXIS_COUNT][8];
    size_t count = 0;
    size_t i;
    bool have_names = false;
    ncl_json *object = NULL;
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;

    for (i = 0; i < (size_t)NCL_FOCAS_AXIS_COUNT; i++) {
        snprintf(names[i], sizeof(names[i]), "%s", kFallback[i]);
    }
    params = ncl_json_new_object();
    if (params != NULL) {
        (void)ncl_json_obj_set_string(params, "item", "AXISNAME");
        (void)ncl_json_obj_set_int(params, "block", 0);
        if (ncl_focas_call(focas, "payload", params, &answer) == NCL_OK) {
            bytes = ncl_json_obj_get(answer, "bytes");
            if (bytes != NULL && ncl_json_type_of(bytes) == NCL_JSON_ARRAY) {
                size_t n = ncl_json_arr_len(bytes) / 4u;

                if (n > (size_t)NCL_FOCAS_AXIS_COUNT) {
                    n = (size_t)NCL_FOCAS_AXIS_COUNT;
                }
                for (i = 0; i < n; i++) {
                    uint8_t c0 = bytes_at(bytes, i * 4u);
                    uint8_t c1 = bytes_at(bytes, i * 4u + 1u);

                    if (c0 == 0) {
                        break;
                    }
                    names[i][0] = (char)(c0 >= 'A' && c0 <= 'Z' ? c0 + 32 : c0);
                    names[i][1] = (char)(c1 >= 'A' && c1 <= 'Z' ? c1 + 32 : c1);
                    names[i][2] = '\0';
                    have_names = true;
                }
                count = i;
            }
        }
        ncl_json_free(params);
        ncl_json_free(answer);
    }
    if (count == 0 && !have_names) {
        /* 轴名表读不到：退回 X/Y/Z/A/C 的顺序，但轴数还得问一句 */
        if (axis_count(focas, &count) != NCL_OK || count == 0) {
            count = sizeof(kFallback) / sizeof(kFallback[0]);
        }
        if (count > (size_t)NCL_FOCAS_AXIS_COUNT) {
            count = (size_t)NCL_FOCAS_AXIS_COUNT;
        }
    }
    object = ncl_json_new_object();
    if (object == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < count; i++) {
        int32_t data = 0;
        int dec = 0;

        if (!record_read(records, i, &data, &dec)) {
            break;
        }
        (void)ncl_json_obj_set_double(object, names[i], record_scale(data, dec));
    }
    if (ncl_json_obj_len(object) == 0) {
        ncl_json_free(object);
        return note(focas, "工件坐标系", NCL_ERR_NOT_FOUND);
    }
    *value = object;
    return NCL_OK;
}

ncl_err ncl_focas_work_offset(ncl_focas *focas, const char *name,
                              ncl_json **value)
{
    ncl_json *records = NULL;
    ncl_json *axes = NULL;
    long long number;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_str_is_blank(name)) {
        return note(focas, "WORK_OFFSET", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    number = zofs_number(name);
    if (number < 0) {
        return note(focas, "工件坐标系", NCL_ERR_INVALID_ARG);
    }
    rc = zofs_read(focas, number, &records);
    if (rc != NCL_OK) {
        return rc;
    }
    rc = zofs_axes_json(focas, records, &axes);
    ncl_json_free(records);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_obj_set_int(axes, "number", number);
    *value = axes;
    return NCL_OK;
}

/* 当前模态（cnc_rdgcode）：T/B/S/F 与一组 G 代码，帧待抓包。 */
/*
 * 模态（`cnc_rdgcode`，item `RDGCODE` = 一个 `0x96`，**d = 第几组**）：
 * 每一组回 12 字节，**代码在 @6（BE16，值是码 ×10）**；机床对 0..23 组都答
 * （24 以上回 rc=3）。真机实测：第 8 组 0x0050 → "G80"、第 20 组 0x0083 → "G13.1"、
 * 第 9 组 "G98"、第 10 组 "G50"、第 12 组 "G97"、第 16 组 "G15"。
 *
 * 出门：`{"gcodes":["G00","G80",…]}` —— 只收**这台机床真报了的组**（每组的码为 0 时
 * 也照报 "G00"：那是"该组的当前码就是 G00"）。
 */
#define FOCAS_GCODE_GROUPS 24

/**
 * 渲染一个 G 码：`flag`（载荷 @10）非 0 说明那一格是"带一位小数"的（码 = 值 ×10），
 * 否则就是整数 —— 真机 24 组全对着看过：
 *
 *     flag=0 → 17 ⇒ G17、40 ⇒ G40、54 ⇒ G54、80 ⇒ G80、98 ⇒ G98、160 ⇒ G160
 *     flag=1 → 401 ⇒ G40.1、131 ⇒ G13.1、501 ⇒ G50.1、542 ⇒ G54.2、805 ⇒ G80.5
 */
static void gcode_render(unsigned code, unsigned flag, char *out, size_t cap)
{
    if (flag != 0u) {
        snprintf(out, cap, "G%u.%u", code / 10u, code % 10u);
    } else {
        snprintf(out, cap, "G%u", code);
    }
}

ncl_err ncl_focas_modal(ncl_focas *focas, ncl_json **value)
{
    ncl_json *array = NULL;
    unsigned group;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (group = 0; group < FOCAS_GCODE_GROUPS; group++) {
        ncl_json *params = NULL;
        ncl_json *answer = NULL;
        ncl_json *bytes = NULL;
        ncl_json *entry = NULL;
        char text[16];
        ncl_err rc;

        params = ncl_json_new_object();
        if (params == NULL) {
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_string(params, "item", "RDGCODE");
        (void)ncl_json_obj_set_int(params, "block", 0);
        (void)ncl_json_obj_set_int(params, "d", (long long)group);
        rc = ncl_focas_call(focas, "payload", params, &answer);
        ncl_json_free(params);
        if (rc == NCL_FOCAS_ERR_NO_DATA) {
            continue; /* 这一组机床没有 */
        }
        if (rc != NCL_OK) {
            ncl_json_free(array);
            return rc;
        }
        bytes = ncl_json_obj_get(answer, "bytes");
        if (bytes == NULL || ncl_json_arr_len(bytes) < 8) {
            ncl_json_free(answer);
            continue;
        }
        gcode_render((unsigned)(((unsigned)bytes_at(bytes, 6) << 8) |
                                (unsigned)bytes_at(bytes, 7)),
                     (unsigned)(((unsigned)bytes_at(bytes, 10) << 8) |
                                (unsigned)bytes_at(bytes, 11)),
                     text, sizeof(text));
        ncl_json_free(answer);
        entry = ncl_json_new_string(text);
        if (entry == NULL || ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
    }
    *value = array;
    return NCL_OK;
}

/*
 * 系统信息（`cnc_sysinfo`）—— 型号 / 系列 / 版本 / 轴数都在 ODBSYS 里：
 *
 *     [0..2)  addinfo   (BE16：bit0 有上料器 / bit1 是 i 系列 / bit8..15 MODEL A..F)
 *     [2..4)  max_axis  (BE16，最大控制轴数)
 *     [4..6)  cnc_type  (ASCII，如 " 0" = Series 0i、"30" = 30i)
 *     [6..8)  mt_type   (ASCII，如 " M" = 加工中心、" T" = 车床)
 *     [8..12) series    (ASCII)
 *     [12..16) version  (ASCII，如 "28.0")
 *     [16..18) axes     (ASCII，当前控制轴数，如 "03")
 *
 * **它不在数据帧里，是会话探针的应答**：`func 0x21` + 一个 `code 24` 的块
 * （item `ODBSYS`）。2026-09 真机（0i-MD）实测：那条回 rc=0、载荷 18 字节
 * `06 62 00 20 " 0" " M" "D4G3" "28.0" "03"`；而老写法那条 `0x0e` +
 * `d=e=0x26f0`（item `VERSION`）被机床拒（rc=1）。所以先用 `ODBSYS`，只有机床
 * **明确拒了**那个码（块返回码非 0）才退到 `VERSION` 那条 —— 别的机型说不定只认
 * 后者（FS0i 那版 `libfwlib32.so` 就是这么发的）。
 */
static ncl_err odbsys_payload(ncl_focas *focas, int len, ncl_json **out)
{
    ncl_err rc = ncl_focas_read_item(focas, "ODBSYS", 0, len, NCL_DTYPE_BYTE, out);

    if (rc == NCL_FOCAS_ERR_RB_CODE) {
        rc = ncl_focas_read_item(focas, "VERSION", 0, len, NCL_DTYPE_BYTE, out);
    }
    return rc;
}

/** ODBSYS 里的一段 ASCII：空格补齐，两头都去掉（`" 0"` → `"0"`、"D4G3" 原样）。 */
static void odbsys_text(const ncl_json *payload, size_t at, size_t len, char *out,
                        size_t cap)
{
    size_t used = 0;
    size_t i;

    if (cap == 0) {
        return;
    }
    for (i = 0; i < len && used + 1u < cap; i++) {
        unsigned byte = bytes_at(payload, at + i);

        if (byte == 0 || byte == ' ') {
            if (used > 0) {
                break; /* 补齐的空格：从第一个空格起就不看了 */
            }
            continue;
        }
        out[used++] = (char)byte;
    }
    out[used] = '\0';
}

ncl_err ncl_focas_system(ncl_focas *focas, ncl_json **value)
{
    ncl_json *payload = NULL;
    ncl_json *object = NULL;
    char text[8];
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    rc = odbsys_payload(focas, 18, &payload);
    if (rc != NCL_OK) {
        return rc;
    }
    object = ncl_json_new_object();
    if (object == NULL) {
        ncl_json_free(payload);
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_int(object, "addinfo",
                               (long long)bytes_at(payload, 0) * 256 +
                                   (long long)bytes_at(payload, 1));
    (void)ncl_json_obj_set_int(object, "maxAxis",
                               (long long)bytes_at(payload, 2) * 256 +
                                   (long long)bytes_at(payload, 3));
    odbsys_text(payload, 4, 2, text, sizeof(text));
    (void)ncl_json_obj_set_string(object, "cncType", text);
    odbsys_text(payload, 6, 2, text, sizeof(text));
    (void)ncl_json_obj_set_string(object, "machineType", text);
    odbsys_text(payload, 8, 4, text, sizeof(text));
    (void)ncl_json_obj_set_string(object, "series", text);
    odbsys_text(payload, 12, 4, text, sizeof(text));
    (void)ncl_json_obj_set_string(object, "version", text);
    odbsys_text(payload, 16, 2, text, sizeof(text));
    (void)ncl_json_obj_set_string(object, "axes", text);
    ncl_json_free(payload);
    *value = object;
    return NCL_OK;
}

/* ------------------------------------------------------------ 程序上下行 -- */

/** 程序上下行里那个目录/文件名的上限（start 帧的体是 516 字节，扣掉 6 字节头）。 */
#define FOCAS_PATH_MAX 509u


/*
 * 程序的上下行不是"读一个 item"，是三件套（01 册 §2.4 与 §11.14，官方 SDK 实测）：
 *
 *   下行（PC → CNC）  cnc_dwnstart4（func 0x11，定长 516 字节体：数据种类 +
 *                     **目录名**）→ 分块 cnc_download4（func 0x12、dir 4，
 *                     体就是程序文本）→ cnc_dwnend4（func 0x13；**下载的错误
 *                     都在这条上回**）
 *   上行（CNC → PC）  cnc_upstart4（0x15）→ cnc_upload4（0x18、dir 4）→ cnc_upend4
 *
 * **两条现场口径**（2026-09-23 用户给的 + 官方 SDK 的帧对出来的）：
 *
 *   1. start 帧那一格给的是**文件夹**（`//CNC_MEM/USER/PATH1/`），**不是文件名** ——
 *      文件名/目录名那一格 SDK 传的就是目录；传成文件路径机床直接回 `EW_ATTRIB=5`。
 *   2. **程序正文的第一行必须是程序号**（`O0001` / `O00001` 都行，FANUC 的规矩），
 *      机床是从这一行认程序号的；正文里没有这一行就是一份机床会拒的程序，
 *      这里先挡下来（回 `NCL_ERR_INVALID_ARG`），别把注定被拒的帧送出去。
 *
 * 帧在驱动层（focas_driver.c 的 "download" / "upload" 操作），这里只管语义与
 * 参数；`type` 的取值照官方手册：0 NC 程序 / 1 刀补 / 2 参数 / 3 螺距误差 /
 * 4 宏变量 / 5 工件零点偏置。
 */

/**
 * 正文的第一行是不是程序号（`O` + 数字，大小写都认；后面可以跟注释）。
 * 空行跳过（有的后处理会先来一个空行）。
 */
static bool program_has_number_line(const char *program)
{
    size_t i = 0;

    while (program[i] == '\r' || program[i] == '\n' || program[i] == ' ') {
        i++;
    }
    if (program[i] != 'O' && program[i] != 'o') {
        return false;
    }
    i++;
    if (program[i] < '0' || program[i] > '9') {
        return false;
    }
    return true;
}

/**
 * 把"目录/文件名"里**目录那一段**取出来（start 帧要的是目录）。
 * `//CNC_MEM/USER/PATH1/O0001` → `//CNC_MEM/USER/PATH1/`；
 * 本来就是目录（以 `/` 结尾）或没有 `/` 就原样给。
 */
static void program_dir_part(const char *name, char *out, size_t cap)
{
    const char *slash;
    size_t len;

    out[0] = '\0';
    if (ncl_str_is_blank(name)) {
        return;
    }
    slash = strrchr(name, '/');
    if (slash == NULL || slash[1] == '\0') {
        snprintf(out, cap, "%s", name);
        return;
    }
    len = (size_t)(slash - name) + 1u; /* 含那个 '/' */
    if (len >= cap) {
        len = cap - 1u;
    }
    memcpy(out, name, len);
    out[len] = '\0';
}

/**
 * 把正文整成机床真收的那份格式（官方 spec `cnc_download4.xml` 的 "NC data format"）：
 *
 *     LF Block1 LF Block2 LF ... LF %
 *
 *   - "**'LF' must be placed at the top of the whole program, and '%' at the end.
 *     Data before the first 'LF' are ignored.**" —— 所以开头那个 LF 不能少；
 *   - "**In case of NC program, address 'O' and program number must be placed in
 *     the program to be registered.**" —— 所以正文里得有 `O<号>` 那一行；
 *   - 它给的例子就是 `"\nO1234\nG1F0.3W10.\nM30\n%"`。
 *
 * 调用方给一份**普通的程序文件**就行：这里补开头的 LF 与结尾的 `%`（已经有了就不重复
 * 补），并校验 `O` 号那一行（`program_has_number_line`）。2026-09-23 对 NCGuide 0i-MF
 * 实测：少了这两样，机床在 `cnc_dwnend4` 回 `EW_ATTRIB=5`、程序**写不进去**；
 * 补齐之后 `end4 rc=0`、程序立刻出现在程序目录里（§11.15）。
 */
static ncl_err program_frame(const char *program, char *out, size_t cap)
{
    size_t len;
    size_t at = 0;
    bool has_tail = false;
    size_t end;

    if (!program_has_number_line(program)) {
        return NCL_ERR_INVALID_ARG;
    }
    len = strlen(program);
    /* 结尾是不是已经有那个 '%'（允许后面跟空白）。 */
    end = len;
    while (end > 0 && (program[end - 1] == '\n' || program[end - 1] == '\r' ||
                       program[end - 1] == ' ')) {
        end--;
    }
    has_tail = end > 0 && program[end - 1] == '%';

    if (program[0] != '\n') {          /* 开头的 LF：机床靠它认"程序从这儿开始" */
        if (cap < 2u) {
            return NCL_ERR_RANGE;
        }
        out[at++] = '\n';
    }
    /* 还要放得下结尾的 '\n'、'%' 和 NUL。 */
    if (at + len + 3u > cap) {
        return NCL_ERR_RANGE;
    }
    memcpy(out + at, program, len);
    at += len;
    if (!has_tail) {
        if (at > 0 && out[at - 1] != '\n') {
            out[at++] = '\n';
        }
        out[at++] = '%';
    }
    out[at] = '\0';
    return NCL_OK;
}

ncl_err ncl_focas_program_download(ncl_focas *focas, long long type,
                                   const char *dir, const char *program)
{
    ncl_json *params;
    ncl_json *result = NULL;
    char target[FOCAS_PATH_MAX];
    char *framed = NULL;
    const char *data = program;
    ncl_err rc;

    if (focas == NULL || program == NULL || program[0] == '\0') {
        return NCL_ERR_INVALID_ARG;
    }
    if (type < 0 || type > 255) {
        return note(focas, "DWNSTART4", NCL_ERR_INVALID_ARG);
    }
    if (type == 0) {
        /*
         * NC 程序：按官方 spec 的格式补齐（开头的 LF + 结尾的 `%`），并校验程序号行。
         * 只有 type 0 走这一套 —— type 1..5 送的是刀补/参数/宏变量，那些正文当然
         * 不是程序。
         */
        size_t need = strlen(program) + 4u;

        framed = (char *)ncl_mem_calloc(need, 1u);
        if (framed == NULL) {
            return NCL_ERR_NOMEM;
        }
        rc = program_frame(program, framed, need);
        if (rc != NCL_OK) {
            ncl_mem_free(framed);
            return note(focas, "PROGRAM_DOWNLOAD", rc);
        }
        data = framed;
    }
    program_dir_part(dir, target, sizeof(target));
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_int(params, "type", type);
    if (!ncl_str_is_blank(target)) {
        (void)ncl_json_obj_set_string(params, "dir", target);
    }
    (void)ncl_json_obj_set_string(params, "data", data);
    rc = ncl_focas_call(focas, "download", params, &result);
    ncl_json_free(params);
    ncl_json_free(result);
    ncl_mem_free(framed);
    if (rc != NCL_OK) {
        return note(focas, "PROGRAM_DOWNLOAD", rc);
    }
    return NCL_OK;
}

/*
 * 上行（CNC → PC）：请求帧**已经核完**（官方 SDK + spec，01 册 §11.14/§11.15）：
 *
 *   cnc_upstart4(h, 0, file_name) → func 0x15、体与下行同形那 516 字节（`[1]` = 种类、
 *                                  `[4..6)` = "N:"、`[6..)` = 文件名/目录/路径+文件名）
 *   cnc_upload4(h, &len, buf)     → func 0x18、dir 4、体 8 字节（全 0）
 *   cnc_upend4(h)                 → 收尾
 *   `file_name` 三种写法（`O1234` / `//CNC_MEM/USER/PATH1/` / `//CNC_MEM/USER/PATH1/O1234`）
 *   都试过；`*len` 按 spec 要 ≥ 256 且是 256 的倍数。读回来的文本是 `% LF Block… LF %`，
 *   最后一个字符是 `%`（再读就是 `EW_RESET`）。
 *
 * **卡在哪**：这台 NCGuide 模拟器收下 start（回 256 字节），但对 `0x18` 那条
 * **一声不响**（SDK 自己回 `EW_DATA=10`；`cnc_getdtailerr` 的细码是 0，即机床没给
 * 任何理由）。上行的三代（`cnc_upstart`/`cnc_upstart3`/`cnc_upstart4`）、三种
 * `file_name` 写法、选主程序、EDIT/MDI 都试过 —— 一样不答。
 *
 * **所以改用"按文件名按行读"这条**（`cnc_rdpdf_line` = Cb `0xf0`，见 §11.16）：
 * 机床**答**这条，而且回的正是程序正文。`ncl_focas_program_upload()` 就是这么实现的
 * —— 一次要 `FOCAS_PDF_LINES_PER_CALL` 行、按回来的行数往后接着读，直到读到空，
 * 把各段拼起来交给上层（读不动就如实回错，不编内容）。
 */
ncl_err ncl_focas_program_upload(ncl_focas *focas, long long type,
                                 const char *name, char **program, size_t *len)
{
    char path[FOCAS_PDF_PATH_SIZE];
    char *text = NULL;
    size_t used = 0;
    long long line = 0;
    ncl_err rc = NCL_OK;

    if (focas == NULL || program == NULL || ncl_str_is_blank(name)) {
        return NCL_ERR_INVALID_ARG;
    }
    *program = NULL;
    if (len != NULL) {
        *len = 0;
    }
    if (type != 0) {
        /* 只有 NC 程序能这么读（刀补/参数/宏变量那几张表另有读法）。 */
        return note(focas, "程序上传", NCL_ERR_UNAVAILABLE);
    }
    /* 路径要"盘名+路径+文件名"；只给文件名就补上默认文件夹（0i 的用户区）。 */
    if (strchr(name, '/') == NULL) {
        snprintf(path, sizeof(path), "//CNC_MEM/USER/PATH1/%s", name);
    } else {
        snprintf(path, sizeof(path), "%s", name);
    }
    text = (char *)ncl_mem_calloc(FOCAS_PDF_PROGRAM_MAX, 1u);
    if (text == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (;;) {
        ncl_json *one = NULL;
        const char *chunk;
        long long lines;

        rc = program_read_lines(focas, path, line, FOCAS_PDF_LINES_PER_CALL,
                                &one);
        if (rc != NCL_OK) {
            if (used > 0) {
                /*
                 * 已经读到东西了，再往后读机床回错（真机上读完是 `EW_DATA`：
                 * "行号超过登记行数"）—— 那就是**读到头**了，把拿到的交出去。
                 */
                ncl_json_free(one);
                rc = NCL_OK;
            }
            break;
        }
        chunk = ncl_json_obj_get_string(one, "text");
        if (chunk == NULL) {
            ncl_json_free(one);
            break; /* 读到头了 */
        }
        {
            size_t n = strlen(chunk);

            if (n == 0) {
                ncl_json_free(one);
                break; /* 空载荷：读到头了 */
            }
            if (used + n + 1u > FOCAS_PDF_PROGRAM_MAX) {
                ncl_json_free(one);
                rc = note(focas, "程序上传", NCL_ERR_RANGE);
                break;
            }
            memcpy(text + used, chunk, n);
            used += n;
            /*
             * 推进的**行数自己数**：机床那份 `cnc_pdf_add` 建出来的程序内容是
             * `O2200%`（末尾 `%`、**没有换行**），数出来是 0 行 —— 所以"读到没读到"
             * 只能看**文字长度**，行数只用来推进（没进过行就说明到结尾了，别绕圈）。
             */
            lines = program_count_lines(chunk, n);
            if (lines > 0) {
                line += lines;
            }
            ncl_json_free(one);
            if (chunk[n - 1u] != '\n') {
                break; /* 末行没有 EOB：这一段就是程序结尾 */
            }
            if (lines == 0) {
                break; /* 保险：一行都没进，别再问同一段 */
            }
            continue;
        }
    }
    if (rc != NCL_OK) {
        ncl_mem_free(text);
        return rc;
    }
    if (used == 0) {
        ncl_mem_free(text);
        /* 一段正文都没读到：这台没有这个程序（或路径不对）—— 如实报"没有"。 */
        return note(focas, "程序上传", NCL_ERR_NOT_FOUND);
    }
    *program = text;
    if (len != NULL) {
        *len = used;
    }
    return NCL_OK;
}

/* --------------------------------------------------- 表 7 的那几种表/对象 -- */

/**
 * 一段宏变量：`cnc_rdmacror`（**码还是 0x15**，只是 `d` = **起始号**、`e` = **结束号**
 * 而不是"号/号"）。2026-09-23 抓帧（01 册 §11.20）：
 *
 *     >> code=0x15 [d=1][e=5] → 载荷 40 字节 = **5 条 8 字节记录**（一条一个号）
 *     >> code=0x15 [d=100][e=100] → 载荷 8 字节 = 号 100 那一条
 *
 * 记录形状与单条读**完全一样**（`[值 BE32][00 0a][dec BE16]`），第 j 条就是
 * `first + j` 号。**空号**（spec：`mcr_val = 0` 且 `dec_val = -1`）在这里如实跳过 ——
 * "vacant"不是 0。
 */
static ncl_err macro_range(ncl_focas *focas, long long first, long long last,
                           ncl_json **records)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_err rc;

    *records = NULL;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDMACRO");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", first);
    (void)ncl_json_obj_set_int(params, "e", last);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        return note(focas, "RDMACRO", NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < (long long)FOCAS_AXIS_RECORD) {
        ncl_json_free(answer);
        return note(focas, "RDMACRO", NCL_ERR_NOT_FOUND);
    }
    *records = ncl_json_clone(bytes);
    ncl_json_free(answer);
    return *records != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/** 一段宏变量 → `{ "号": 值, … }`（空号与读不到的号都不列）。 */
ncl_err ncl_focas_macro_variables(ncl_focas *focas, long long first,
                                  long long count, ncl_json **value)
{
    ncl_json *records = NULL;
    ncl_json *object = NULL;
    size_t i;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (first < 0 || count <= 0) {
        return note(focas, "RDMACRO", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    rc = macro_range(focas, first, first + count - 1, &records);
    if (rc != NCL_OK) {
        return rc;
    }
    object = ncl_json_new_object();
    if (object == NULL) {
        ncl_json_free(records);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < (size_t)count; i++) {
        int32_t data = 0;
        int dec = 0;
        char key[24];

        if (!record_read(records, i, &data, &dec)) {
            break;
        }
        if (record_is_vacant(records, i)) {
            continue; /* vacant（spec：值 0 + dec -1）= 这个号没定义 */
        }
        snprintf(key, sizeof(key), "%lld", first + (long long)i);
        (void)ncl_json_obj_set_double(object, key, record_scale(data, dec));
    }
    ncl_json_free(records);
    if (ncl_json_obj_len(object) == 0) {
        ncl_json_free(object);
        return note(focas, "RDMACRO", NCL_ERR_NOT_FOUND);
    }
    *value = object;
    return NCL_OK;
}

/*
 * 参数表（表 6 的 PARAMETER，dict）：**范围读**（`cnc_rdparar` 复用 0x8d 那条码，
 * 段的三个数落在 Cb 的 d/e/arg2 —— `d = 起始号、e = 类型、arg2 = 结束号、
 * a3 = 结束类型`，2026-09 从官方 SDK 的帧上核的，01 册 §11.13）。
 *
 * 这台 0i-MF 上一段能读多宽、以及"机床回几条记录"还没逐段核过；这里按**一段
 * 一条**读、每段 `FOCAS_PARAM_TABLE_CHUNK` 个号，读到读不动为止 —— 读不动就
 * 把已经拿到的交出去，一条都没有才报错（不交空表，也不假装读全了）。
 */
#define FOCAS_PARAM_TABLE_CHUNK 8
#define FOCAS_PARAM_TABLE_MAX   64

ncl_err ncl_focas_parameter_table(ncl_focas *focas, ncl_json **value)
{
    ncl_json *table;
    long long start;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    table = ncl_json_new_object();
    if (table == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (start = 1; start <= FOCAS_PARAM_TABLE_MAX;
         start += FOCAS_PARAM_TABLE_CHUNK) {
        long long offset;
        bool any = false;

        for (offset = 0; offset < FOCAS_PARAM_TABLE_CHUNK; offset++) {
            ncl_json *one = NULL;
            char key[24];
            ncl_err rc = record_call(focas, "RDPARAM", start + offset,
                                     0, "RDPARAM", &one);

            if (rc != NCL_OK) {
                continue; /* 这个号读不到：跳过，别的号还能读 */
            }
            snprintf(key, sizeof(key), "%lld", start + offset);
        if (ncl_json_obj_set(table, key, ncl_json_clone(one)) != NCL_OK) {
                ncl_json_free(one);
                ncl_json_free(table);
                return NCL_ERR_NOMEM;
            }
            ncl_json_free(one);
            any = true;
        }
        if (!any) {
            break; /* 这一段一个都没读到：到头了 */
        }
    }
    if (ncl_json_obj_len(table) == 0) {
        ncl_json_free(table);
        return note(focas, "参数表", NCL_ERR_NOT_FOUND);
    }
    *value = table;
    return NCL_OK;
}

/**
 * 宏变量表（表 7 的 VARIABLE，list）：按**号段**读（`cnc_rdmacror`，一次一段），
 * 只列**已定义**的号 —— 空号（vacant）与读不到的号都不进表（表 4 那句是"运行变量"，
 * 空号不是运行变量；也不硬凑一个 0）。
 *
 * 段宽取 `FOCAS_MACRO_TABLE_CHUNK`：本机一次要太多会拒/截断（实测 `d=33 e=64`
 * 只回 1 条、`d=1 e=40` 回 33 条 = 264 字节封顶），所以按 16 个号一段问，
 * 段里"第 j 条记录就是 `起始号 + j`"。
 */
#define FOCAS_MACRO_TABLE_CHUNK 16
#define FOCAS_MACRO_TABLE_MAX   999 /**< 用户宏变量 #1..#999 */

ncl_err ncl_focas_variable_table(ncl_focas *focas, ncl_json **value)
{
    ncl_json *table = NULL;
    long long start;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    table = ncl_json_new_object();
    if (table == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (start = 1; start <= FOCAS_MACRO_TABLE_MAX;
         start += FOCAS_MACRO_TABLE_CHUNK) {
        long long last = start + FOCAS_MACRO_TABLE_CHUNK - 1;
        ncl_json *records = NULL;
        size_t i;
        ncl_err rc = macro_range(focas, start, last, &records);

        if (rc != NCL_OK) {
            continue; /* 这一段机床不收：跳过，别的段照读 */
        }
        for (i = 0; i < (size_t)FOCAS_MACRO_TABLE_CHUNK; i++) {
            int32_t data = 0;
            int dec = 0;
            char key[24];

            if (!record_read(records, i, &data, &dec)) {
                break; /* 机床回的比要的少：这一段的剩余部分不猜 */
            }
            if (record_is_vacant(records, i)) {
                continue; /* vacant = 没定义 */
            }
            snprintf(key, sizeof(key), "%lld", start + (long long)i);
            if (ncl_json_obj_set_double(table, key, record_scale(data, dec)) !=
                NCL_OK) {
                ncl_json_free(records);
                ncl_json_free(table);
                return NCL_ERR_NOMEM;
            }
        }
        ncl_json_free(records);
    }
    if (ncl_json_obj_len(table) == 0) {
        ncl_json_free(table);
        return note(focas, "宏变量表", NCL_ERR_NOT_FOUND);
    }
    *value = table;
    return NCL_OK;
}

/** 整张坐标系表：外部 + G54…G59（每个号一次全轴读）。 */
ncl_err ncl_focas_work_offsets(ncl_focas *focas, ncl_json **value)
{
    ncl_json *table;
    long long number;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    table = ncl_json_new_object();
    if (table == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (number = 0; number <= 6; number++) { /* 0 = 外部，1..6 = G54..G59 */
        ncl_json *records = NULL;
        ncl_json *axes = NULL;
        char name[24];
        ncl_err rc = zofs_read(focas, number, &records);

        if (rc != NCL_OK) {
            if (number == 0) {
                ncl_json_free(table);
                return rc;
            }
            continue; /* 这个号读不到（机型没有）：跳过，别的号照读 */
        }
        rc = zofs_axes_json(focas, records, &axes);
        ncl_json_free(records);
        if (rc != NCL_OK) {
            continue;
        }
        zofs_name(number, name, sizeof(name));
        if (ncl_json_obj_set(table, name, axes) != NCL_OK) {
            ncl_json_free(axes);
            ncl_json_free(table);
            return NCL_ERR_NOMEM;
        }
    }
    if (ncl_json_obj_len(table) == 0) {
        ncl_json_free(table);
        return note(focas, "工件坐标系", NCL_ERR_NOT_FOUND);
    }
    *value = table;
    return NCL_OK;
}

/**
 * 写一个轴的工件零点偏移（`cnc_wrzofs` = item `WRZOFS` = **0x0c**）。
 *
 * 载荷与写刀补/宏变量同形（`[值 BE32][00 00][ff ff]`），值是**最低输入单位**的整数
 * —— 所以先读一次拿这台机床的小数位（`dec`），`raw = value × 10^dec`。一次只写一根
 * 轴（`arg2` = 轴号，1 起）；`axis` 是**机床的轴号**（1 = X…），与记录里的第几根轴一致。
 */
ncl_err ncl_focas_work_offset_write(ncl_focas *focas, const char *name,
                                    long long axis_number, double value)
{
    ncl_json *records = NULL;
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    int32_t current = 0;
    int dec = 0;
    int32_t raw;
    long long number;
    size_t i;
    ncl_err rc;

    if (focas == NULL || ncl_str_is_blank(name) || axis_number < 1) {
        return NCL_ERR_INVALID_ARG;
    }
    number = zofs_number(name);
    if (number < 0) {
        return note(focas, "写工件坐标系", NCL_ERR_INVALID_ARG);
    }
    rc = zofs_read(focas, number, &records);
    if (rc != NCL_OK) {
        return rc;
    }
    if (!record_read(records, (size_t)(axis_number - 1), &current, &dec)) {
        ncl_json_free(records);
        return note(focas, "写工件坐标系", NCL_ERR_RANGE); /* 这台没有这根轴 */
    }
    ncl_json_free(records);
    if (value > 2147483.0 || value < -2147483.0) {
        return note(focas, "写工件坐标系", NCL_ERR_RANGE);
    }
    raw = (int32_t)(value * tofs_scale(dec) + (value < 0.0 ? -0.5 : 0.5));
    params = ncl_json_new_object();
    bytes = ncl_json_new_array();
    if (params == NULL || bytes == NULL) {
        ncl_json_free(params);
        ncl_json_free(bytes);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < 4u; i++) {
        long long byte = ((uint32_t)raw >> (8 * (3u - i))) & 0xFF;

        if (ncl_json_arr_push(bytes, ncl_json_new_int(byte)) != NCL_OK) {
            ncl_json_free(params);
            ncl_json_free(bytes);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0));
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0));
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0xFF));
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0xFF));
    (void)ncl_json_obj_set_string(params, "item", "WRZOFS");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number);
    (void)ncl_json_obj_set_int(params, "arg2", axis_number);
    (void)ncl_json_obj_set(params, "data", bytes);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "写工件坐标系", rc);
    }
    /*
     * 写后复核：比**原始整数**（不跟小数位纠缠）——没落到位就如实回"这台写不进去"，
     * 与写参数/写宏变量同一个口径。
     *
     * **要重试几次**：机床把值落到偏移表上不是"回了 rc=0 就立刻读得到"
     * （2026-09-23 实测：紧跟着读回的是**旧值**，隔一会儿再读才是新值），所以这里
     * 读几遍、每遍隔 50 ms；一直不变才回"没写进去"。别因为读得太早把成功报成失败。
     */
    {
        unsigned attempt;

        for (attempt = 0; attempt < 6u; attempt++) {
            ncl_json *back = NULL;
            int32_t after = 0;
            int after_dec = 0;

            rc = zofs_read(focas, number, &back);
            if (rc != NCL_OK) {
                return rc;
            }
            if (!record_read(back, (size_t)(axis_number - 1), &after,
                             &after_dec)) {
                ncl_json_free(back);
                return note(focas, "写工件坐标系", NCL_ERR_PARSE);
            }
            ncl_json_free(back);
            if (after == raw) {
                return NCL_OK;
            }
            if (attempt + 1u < 6u) {
                ncl_sleep_millis(50u);
            }
        }
        return note(focas, "写工件坐标系", NCL_ERR_UNAVAILABLE);
    }
}

/*
 * 机床型号 / 系统版本：FOCAS **没有**单独的"型号"调用（`cnc_rdmodel` 连官方文档包
 * 里都没有），型号信息在 `ODBSYS` 里 —— 也就是连接期那条能力块（Cb `0x0e`，
 * `d = e = 0x26f0`），跟 `cnc_sysinfo` 是同一个结构（官方头 / 文档 SpecE Misc/
 * cnc_sysinfo.xml）：
 *
 *     [0..2)  addinfo   (BE16：bit0 有上料器 / bit1 是 i 系列 / bit8..15 MODEL A..F)
 *     [2..4)  max_axis  (BE16，最大控制轴数)
 *     [4..6)  cnc_type  (ASCII，如 " 0" = Series 0i、'30' = 30i)
 *     [6..8)  mt_type   (ASCII，如 " M" = 加工中心、" T" = 车床)
 *     [8..12) series    (ASCII)
 *     [12..16) version  (ASCII，如 "49.0")
 *     [16..18) axes     (ASCII，当前控制轴数，如 "03")
 *
 * 这里的口径（本实现的约定，站点可以自己换）：
 *   `MODEL`   = `cnc_type` + `mt_type` 去掉补齐的空格，再接 " " + `series`
 *   `VERSION` = `version`
 * 想换拼法的直接读 item `VERSION` 的 `@4`/`@6`/`@8`/`@12` 那几格（名字保留 `VERSION`
 * 是因为它就是连接期那条 0x0e 能力块）。
 */
static ncl_err odbsys_field(ncl_focas *focas, size_t at, size_t len, char *out,
                            size_t cap, const char *what)
{
    ncl_json *payload = NULL;
    ncl_err rc;

    if (len + 1u > cap) {
        return note(focas, what, NCL_ERR_RANGE);
    }
    rc = odbsys_payload(focas, 18, &payload);
    if (rc != NCL_OK) {
        return rc;
    }
    odbsys_text(payload, at, len, out, cap);
    ncl_json_free(payload);
    if (out[0] == '\0') {
        return note(focas, what, NCL_ERR_PARSE); /* 机床没报这一格 */
    }
    return NCL_OK;
}

ncl_err ncl_focas_model(ncl_focas *focas, char *out, size_t cap)
{
    char kind[3];
    char mt[3];
    char series[5];
    ncl_err rc;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    rc = odbsys_field(focas, 4, 2, kind, sizeof(kind), "型号");
    if (rc != NCL_OK) {
        return rc;
    }
    rc = odbsys_field(focas, 6, 2, mt, sizeof(mt), "型号");
    if (rc != NCL_OK) {
        return rc;
    }
    rc = odbsys_field(focas, 8, 4, series, sizeof(series), "型号");
    if (rc != NCL_OK) {
        return rc;
    }
    snprintf(out, cap, "%s%s %s", kind, mt, series);
    return NCL_OK;
}

ncl_err ncl_focas_version(ncl_focas *focas, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return odbsys_field(focas, 12, 4, out, cap, "版本");
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

/*
 * 删程序（`cnc_delete` = 一个 **0x05**，d = 程序号：O 后面那个数）。
 *
 * 帧是官方 SDK 抓的（`cnc_delete(h, 1)` → Cb 0x05、d=1）。**这台模拟器回
 * EW_ATTRIB=5**（条 0x05 与 0xb6 两条都是），也就是"机床不收这条" —— 所以这台机器
 * 上删不了程序，但码与形状是官方库里那一条，站点接了真机就能用。
 */
/**
 * 建一个程序文件 / 文件夹（`cnc_pdf_add` = item `RDPDFADD` = **`0xb5`**，
 * `d` = 0 文件 / 1 文件夹，载荷 = 256 字节路径）。
 *
 * 2026-09-23 实测（模拟器）：`//CNC_MEM/USER/PATH1/O1234` 建出来 `rc=0`，而且机床
 * **自动把程序号那一行写进去**（读回来是 `O1234\n`）—— 也就是"建空程序"这一步机床自己会做。
 */
ncl_err ncl_focas_program_create(ncl_focas *focas, const char *name, bool folder)
{
    ncl_json *params;
    ncl_json *data;
    ncl_json *answer = NULL;
    char path[FOCAS_PDF_PATH_SIZE];
    size_t i;
    ncl_err rc;

    if (focas == NULL || ncl_str_is_blank(name)) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(path, 0, sizeof(path));
    snprintf(path, sizeof(path), "%s", name);
    params = ncl_json_new_object();
    data = ncl_json_new_array();
    if (params == NULL || data == NULL) {
        ncl_json_free(params);
        ncl_json_free(data);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < sizeof(path); i++) {
        if (ncl_json_arr_push(data, ncl_json_new_int((unsigned char)path[i])) !=
            NCL_OK) {
            ncl_json_free(params);
            ncl_json_free(data);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_obj_set_string(params, "item", "RDPDFADD");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", folder ? 1 : 0);
    (void)ncl_json_obj_set(params, "data", data);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "建程序", rc);
    }
    return NCL_OK;
}

/*
 * 删程序（`cnc_pdf_del` = item `RDPDFDEL` = **`0xb6`**，载荷 = 256 字节路径）。
 *
 * 2026-09-23 实测（模拟器）：`//CNC_MEM/USER/PATH1/O1234` 删掉 `rc=0`；路径不存在时
 * 回 `EW_ATTRIB`。`name` 收完整路径，也收裸文件名/`O1234`（自动补默认文件夹）。
 *
 * 另有一条按**程序号**删的 `cnc_delete`（Cb `0x05`，item `DELPROG`）—— 官方手册里
 * "任意机型都支持"，但帧记着的这条（0xb6）是实测通过的，所以默认走它。
 */
ncl_err ncl_focas_program_delete(ncl_focas *focas, const char *name)
{
    ncl_json *params;
    ncl_json *data;
    ncl_json *answer = NULL;
    char path[FOCAS_PDF_PATH_SIZE];
    size_t i;
    ncl_err rc;

    if (focas == NULL || ncl_str_is_blank(name)) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(path, 0, sizeof(path));
    if (strchr(name, '/') != NULL) {
        snprintf(path, sizeof(path), "%s", name);
    } else if (name[0] == 'O' || name[0] == 'o') {
        snprintf(path, sizeof(path), "//CNC_MEM/USER/PATH1/%s", name);
    } else {
        snprintf(path, sizeof(path), "//CNC_MEM/USER/PATH1/O%s", name);
    }
    params = ncl_json_new_object();
    data = ncl_json_new_array();
    if (params == NULL || data == NULL) {
        ncl_json_free(params);
        ncl_json_free(data);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < sizeof(path); i++) {
        if (ncl_json_arr_push(data, ncl_json_new_int((unsigned char)path[i])) !=
            NCL_OK) {
            ncl_json_free(params);
            ncl_json_free(data);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_obj_set_string(params, "item", "RDPDFDEL");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", 0);
    (void)ncl_json_obj_set_int(params, "e", 0);
    (void)ncl_json_obj_set(params, "data", data);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "删程序", rc);
    }
    return NCL_OK;
}
/*
 * 写一条 CNC 参数（`cnc_wrparam` 的**第二条帧**，item `WRPARAM2` = **0x8e**）。
 *
 * 2026-09-23 在这台 NCGuide 0i-MF 上**写进去又读回核过**（01 册 §11.26）：
 *
 *     载荷 = 机床自己那条读应答的 **264 字节记录原样**，只把值那一格换掉：
 *         @0..4  号（BE32）、@4..6 轴号（BE16）、@6..8 属性（BE16）、@8..12 值（BE32）
 *     块头四格 d/e/arg2/arg3 **全 0**，tag1 = 264（"载荷挂在块后面"那条规矩）。
 *     机床回"块返回码 0"才算收下；12/8 字节的小载荷一律 EW_LENGTH=2。
 *
 * 三条**踩过的坑**：
 *   ① 第一条帧（`0xa0` 问属性）只是老实现的自作主张 —— 记录里本来就要带轴号和属性，
 *      照读应答回填就行，**不需要先问**；只发第一条时机床回码 0 但值不动。
 *   ② 官方 SDK 的 `cnc_wrparam` 在这台机器上**写不动**：它把 `IODBPSD.type` 那个
 *      short 原样塞进 @4..6（轴号那一格）→ 机床回 EW_ATTRIB；按老口径（type=0、
 *      length=5）发的时候它又把号按**主机序**写进记录 → 机床回 EW_NUMBER。所以这
 *      一条是按机床自己的读应答形状做的，不是照抄 SDK 的帧。
 *   ③ 值按**有符号 32 位**整格写：bit/byte 参数也是整字节写（FOCAS 不允许按位写）。
 *
 * 写权限（参数写入允许 / PWE，机床上是"参数写入"开关）没开时机床回 EW_PROT=7，
 * 这里如实翻成 `NCL_ERR_UNAVAILABLE`。**写完一律读回复核**，对不上就回
 * `NCL_ERR_UNAVAILABLE`，绝不假成功。
 */
static ncl_err parameter_write_axis(ncl_focas *focas, long long number, long long axis,
                                    const char *value)
{
    uint8_t record[NCL_FOCAS_PARAM_RECORD];
    uint8_t check[NCL_FOCAS_PARAM_RECORD];
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *data = NULL;
    uint32_t raw;
    long long want;
    char *end = NULL;
    ncl_err rc;
    size_t i;

    if (focas == NULL || ncl_str_is_blank(value) || number < 0 || axis < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    want = strtoll(value, &end, 0);
    if (end == value) {
        return note(focas, "写参数", NCL_ERR_INVALID_ARG);
    }
    raw = (uint32_t)want;
    /* ① 拿机床自己那条记录（号 / 轴号 / 属性 都在里面）——读不到就是不支持这个号 */
    rc = parameter_record(focas, number, axis, record);
    if (rc != NCL_OK) {
        return rc;
    }
    record[8] = (uint8_t)(raw >> 24);
    record[9] = (uint8_t)(raw >> 16);
    record[10] = (uint8_t)(raw >> 8);
    record[11] = (uint8_t)raw;
    /* ② 264 字节载荷挂上去发 */
    data = ncl_json_new_array();
    params = ncl_json_new_object();
    if (data == NULL || params == NULL) {
        ncl_json_free(data);
        ncl_json_free(params);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < NCL_FOCAS_PARAM_RECORD; i++) {
        if (ncl_json_arr_push(data, ncl_json_new_int(record[i])) != NCL_OK) {
            ncl_json_free(data);
            ncl_json_free(params);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_obj_set_string(params, "item", "WRPARAM2");
    (void)ncl_json_obj_set_int(params, "block", 0);
    /* 块头四格全 0：号和属性在**载荷**里（§11.26） */
    (void)ncl_json_obj_set_int(params, "d", 0);
    (void)ncl_json_obj_set_int(params, "e", 0);
    (void)ncl_json_obj_set(params, "data", data);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "写参数", rc);
    }
    /*
     * ③ 读回复核，**而且要等一会儿**：这台机器的 FOCAS 服务端对参数块有个几百毫秒的
     * 缓存 —— 写完立刻读回拿到的还是旧值（2026-09-23 实测：写 4321 后立刻读 = 5555，
     * 300ms 后再读 = 4321）。所以这里轮询几轮，一直对不上就如实回"写不进去"。
     * 写是管理动作、不频繁，多花这不到一秒换"不假成功"是划算的。
     */
    for (i = 0; i < 5u; i++) {
        if (i > 0) {
            ncl_sleep_millis(150u);
        }
        rc = parameter_record(focas, number, axis, check);
        if (rc != NCL_OK) {
            return rc;
        }
        if ((int32_t)(((uint32_t)check[8] << 24) | ((uint32_t)check[9] << 16) |
                      ((uint32_t)check[10] << 8) | (uint32_t)check[11]) ==
            (int32_t)want) {
            return NCL_OK;
        }
    }
    return note(focas, "写参数", NCL_ERR_UNAVAILABLE);
}

/** 写一个**无轴** CNC 参数。带轴的用 `ncl_focas_parameter_write_axis`。 */
ncl_err ncl_focas_parameter_write(ncl_focas *focas, long long number,
                                  const char *value)
{
    return parameter_write_axis(focas, number, 0, value);
}

/** 写一个**带轴** CNC 参数：@p axis = 1..n，只动那一轴。 */
ncl_err ncl_focas_parameter_write_axis(ncl_focas *focas, long long number,
                                       long long axis, const char *value)
{
    return parameter_write_axis(focas, number, axis, value);
}

/* 写刀补（`cnc_wrtofs`）：**改刀补会导致撞刀**，官方手册专门警告过。
 * 不带类型的那一条写的是 **type 0**（半径磨损）—— 与 `ncl_focas_tool_offset()`
 * 读的那一格是同一格，读写对称。要写别的字段用 `ncl_focas_tool_offset_write_typed`
 * 或 `ncl_focas_tool_param_write()`。 */
ncl_err ncl_focas_tool_offset_write(ncl_focas *focas, long long index,
                                    const char *value)
{
    if (focas == NULL || ncl_str_is_blank(value) || index < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    return tofs_write(focas, index, FOCAS_TOFS_RADIUS_WEAR,
                      strtod(value, NULL));
}

/**
 * 读一条宏变量的**原始记录**（值 + 小数位）。写之前要靠它拿机床自己的 dec：
 * 值是按"最低输入单位"送的，dec 不对值就差 10 倍。
 */
static ncl_err macro_read_raw(ncl_focas *focas, long long number, int32_t *data,
                              int *dec)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_err rc;

    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDMACRO");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number); /* d = e = 号 */
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        return note(focas, "RDMACRO", NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < (long long)FOCAS_AXIS_RECORD) {
        ncl_json_free(answer);
        return note(focas, "RDMACRO", NCL_ERR_NOT_FOUND);
    }
    if (!record_read(bytes, 0, data, dec)) {
        ncl_json_free(answer);
        return note(focas, "RDMACRO", NCL_ERR_PARSE);
    }
    ncl_json_free(answer);
    return NCL_OK;
}

/**
 * 写宏变量（`cnc_wrmacro` = item `WRMACRO` = **0x16**，读 0x15 + 1）。
 *
 * 载荷形状照刀补那条（BE32 值 + BE16 0 + BE16 0xffff）。**这台机床没开用户宏变量**：
 * 读 0x15 回 EW_NOOPT=6，写这条同样被拒 —— 所以帧发出去了、机床怎么答照实往上报
 * （`NCL_FOCAS_ERR_RB_CODE`），不假装写成功。
 */
ncl_err ncl_focas_macro_write(ncl_focas *focas, long long number, double value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    int32_t raw;
    int dec = 0;
    size_t i;
    ncl_err rc;

    if (focas == NULL || number < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    /*
     * 先读一条打底：既有"这一号存不存在"，也拿到**机床自己的小数位** —— 值是按
     * 最低输入单位送的，dec 不对值就差 10 倍（2026-09 实测：不按 dec 缩放时，
     * 写 626 到 501 号、读回来是 6260）。这一条与写刀补同一个口径。
     */
    rc = macro_read_raw(focas, number, &raw, &dec);
    if (rc != NCL_OK) {
        return rc;
    }
    if (value > 2147483647.0 || value < -2147483648.0) {
        return note(focas, "写宏变量", NCL_ERR_RANGE);
    }
    raw = (int32_t)(value * tofs_scale(dec) + (value < 0.0 ? -0.5 : 0.5));
    params = ncl_json_new_object();
    bytes = ncl_json_new_array();
    if (params == NULL || bytes == NULL) {
        ncl_json_free(params);
        ncl_json_free(bytes);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < 4u; i++) {
        long long byte = ((uint32_t)raw >> (8 * (3u - i))) & 0xFF;

        if (ncl_json_arr_push(bytes, ncl_json_new_int(byte)) != NCL_OK) {
            ncl_json_free(params);
            ncl_json_free(bytes);
            return NCL_ERR_NOMEM;
        }
    }
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0));
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0));
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0xFF));
    (void)ncl_json_arr_push(bytes, ncl_json_new_int(0xFF));
    (void)ncl_json_obj_set_string(params, "item", "WRMACRO");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number);
    (void)ncl_json_obj_set(params, "data", bytes);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "写宏变量", rc);
    }
    /*
     * **写后复核**：这台模拟器上 0x16 明明回块返回码 0，值却不一定落到位
     * （2026-09 实测：写 500 号不动、写 501 号读回来是"写进去的 ×10" —— 说明值那一格
     * 的刻度还没跟机床对齐）。所以这里再读一次：读出来的值与要写的不在一个刻度上，
     * 就如实回 `NCL_ERR_UNAVAILABLE`，并把读到的值写进 last_error。**绝不回成功**。
     */
    {
        int32_t back = 0;
        int back_dec = 0;
        double got;
        double slack;

        rc = macro_read_raw(focas, number, &back, &back_dec);
        if (rc != NCL_OK) {
            return rc;
        }
        got = record_scale(back, back_dec);
        slack = 2.0 / tofs_scale(back_dec < 0 ? 0 : back_dec);
        if (got > value + slack || got < value - slack) {
            snprintf(focas->error, sizeof(focas->error),
                     "写宏变量: 机床收了帧但读回来是 %g（值与机床刻度没对齐，按"
                     "\"没写进去\"算）",
                     got);
            return NCL_ERR_UNAVAILABLE;
        }
    }
    return NCL_OK;
}
/* ------------------------------------------------------------ 程序上下行 -- */

/** 程序上下行里那个目录/文件名的上限（start 帧的体是 516 字节，扣掉 6 字节头）。 */
#define FOCAS_PATH_MAX 509u
