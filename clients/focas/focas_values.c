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

/** 记下这次失败（绑定的 NG 理由就是这一句）。 */
static ncl_err note(ncl_focas *focas, const char *what, ncl_err code)
{
    if (focas != NULL) {
        snprintf(focas->error, sizeof(focas->error), "%s: %s", what,
                 focas_err_text(code));
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
    (void)value;
    return not_yet(focas, "当前刀具号",
                   "cnc_rdgcode 的 T 组 / ODBDY2（0x96 只报 G 组，0x2e 那条回整段程序）");
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

ncl_err ncl_focas_program_directory(ncl_focas *focas, ncl_json **value)
{
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_json *array = NULL;
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
    (void)ncl_json_obj_set_string(params, "item", "RDPROGDIR3");
    (void)ncl_json_obj_set_int(params, "block", 0);
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
    array = ncl_json_new_array();
    if (array == NULL) {
        ncl_json_free(answer);
        return NCL_ERR_NOMEM;
    }
    records = length / FOCAS_PROG_RECORD;
    for (i = 0; i < records; i++) {
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
        entry = ncl_json_new_object();
        if (entry == NULL ||
            ncl_json_obj_set_int(entry, "number", number) != NCL_OK ||
            ncl_json_obj_set_string(entry, "comment", comment) != NCL_OK ||
            ncl_json_arr_push(array, entry) != NCL_OK) {
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

/**
 * 一个 CNC 参数（`cnc_rdparam`，item `RDPARAM` = 一个 **`0x8d`**：**d = e = 参数号**、
 * `arg2` = 0）。应答载荷 264 字节，前三格都是 BE32：
 *
 *     @0  参数号（机床把问的号回一遍）
 *     @4  这一号的"条数/长度"（20 号回 1、6711 号回 3、多数回 0）
 *     @8  **值**        ← 官方 SDK 的 `IODBPSD.u.ldata` 就落在这里
 *
 * 这三格是 2026-09-23 用**两个值不为 0 的参数**交叉核出来的（20 号 → 4、6000 号 → 4，
 * 而载荷 @8 恰好都是 4；6711 号 @8 = 0、SDK 报的也是 0）。
 * ⚠️ 以前按载荷 **@0** 当值 —— 那是**参数号本身**，只有 1 号参数看着"对"（1 == 1 巧合），
 * 6711 读出来就是 6711（一眼假）。
 */
ncl_err ncl_focas_parameter(ncl_focas *focas, long long number,
                            ncl_json **value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    int32_t data = 0;
    int dec = 0;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (number < 0) {
        return note(focas, "RDPARAM", NCL_ERR_INVALID_ARG);
    }
    *value = NULL;
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "RDPARAM");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number); /* d = e = 号（见 record_call） */
    (void)ncl_json_obj_set_int(params, "arg2", 0);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    if (rc == NCL_FOCAS_ERR_NO_DATA) {
        return note(focas, "RDPARAM", NCL_ERR_NOT_FOUND);
    }
    if (rc != NCL_OK) {
        return rc;
    }
    bytes = ncl_json_obj_get(answer, "bytes");
    if (bytes == NULL || ncl_json_type_of(bytes) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(bytes) < 12) {
        ncl_json_free(answer);
        return note(focas, "RDPARAM", NCL_ERR_NOT_FOUND); /* 空载荷 = 没有这一号 */
    }
    /* 值 = 载荷 @8 的 BE32（前两格是号与条数）。 */
    data = (int32_t)(((uint32_t)bytes_at(bytes, 8) << 24) |
                     ((uint32_t)bytes_at(bytes, 9) << 16) |
                     ((uint32_t)bytes_at(bytes, 10) << 8) |
                     (uint32_t)bytes_at(bytes, 11));
    (void)dec;
    ncl_json_free(answer);
    *value = ncl_json_new_object();
    if (*value == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_int(*value, "number", number);
    (void)ncl_json_obj_set_double(*value, "value",
                                  (double)data); /* 参数没有小数位那一说 */
    (void)ncl_json_obj_set_int(*value, "raw", (long long)data);
    return NCL_OK;
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

ncl_err ncl_focas_variable_table(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    /*
     * 宏变量表（表 7 的 VARIABLE，list）：`cnc_rdmacror`（0x15 带号段）本来就能读
     * 一段，但**这台机床没开用户宏变量**：读 0x15 回 **EW_NOOPT=6**（01 册 §11.13），
     * 所以这里如实回"机床不提供"，不编一张空表。
     */
    return not_yet(focas, "宏变量表",
                   "cnc_rdmacror（这台机床没开用户宏变量，回 EW_NOOPT=6）");
}

ncl_err ncl_focas_work_offsets(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    return not_yet(focas, "工件坐标系", "cnc_rdwkcdshft 一族（G54…）");
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
ncl_err ncl_focas_program_delete(ncl_focas *focas, const char *name)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    long long number = 0;
    const char *p;
    ncl_err rc;

    if (focas == NULL || ncl_str_is_blank(name)) {
        return NCL_ERR_INVALID_ARG;
    }
    /* "O0001" / "1" / "0001" 都按程序号收；带 '/' 的是路径，这条路不走。 */
    p = name;
    if (*p == 'O' || *p == 'o') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        number = number * 10 + (*p - '0');
        p++;
    }
    if (number <= 0 || *p != '\0') {
        return note(focas, "删程序", NCL_ERR_INVALID_ARG);
    }
    params = ncl_json_new_object();
    if (params == NULL) {
        return NCL_ERR_NOMEM;
    }
    (void)ncl_json_obj_set_string(params, "item", "DELPROG");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", 0);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "删程序", rc);
    }
    return NCL_OK;
}

/**
 * 写参数（`cnc_wrparam` = item `WRPARAM` = **0xa0**）。
 *
 * 官方 SDK 对着这台机器发的就是 0xa0、d = 参数号、e = 1（**不带载荷**），机床回块
 * 返回码 0，可 SDK 自己回 EW_LENGTH=2 —— 值没送出去。这里按写刀补那个形状补一段
 * 8 字节载荷（BE32 值 + BE16 0 + BE16 0xffff）试；**机床收不收由它说了算**，
 * 不收就是模块错（不会假装写成功）。参数号与值的对应（字节/字/双字）也没法在这台
 * 机器上定标 —— 写之前站点点位表必须自己确认这一号是什么量。
 *
 * **写后复核**：写完把这一号再读一次。2026-09 在这台模拟器上，这一帧机床回块返回码
 * 0、**但值没变**（写 0 到 1 号参数，再读还是 1）—— 所以这一条只当"把值送出去"，
 * 真正的判据是复核：值没变就回 `NCL_ERR_UNAVAILABLE`（"这台写不进去"），
 * **绝不回成功**。站点看到这个错表明"参数写还没打通"，而不是"写成功了"。
 */
ncl_err ncl_focas_parameter_write(ncl_focas *focas, long long number,
                                  const char *value)
{
    ncl_json *params;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_json *before = NULL;
    ncl_json *after = NULL;
    char *end = NULL;
    long long raw;
    long long was_raw = 0;
    bool had_raw = false;
    size_t i;
    ncl_err rc;

    if (focas == NULL || ncl_str_is_blank(value) || number < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    raw = strtoll(value, &end, 0);
    if (end == value) {
        return note(focas, "写参数", NCL_ERR_INVALID_ARG);
    }
    rc = ncl_focas_parameter(focas, number, &before);
    if (rc != NCL_OK) {
        ncl_json_free(before);
        return rc;
    }
    had_raw = ncl_json_obj_has(before, "raw");
    was_raw = ncl_json_obj_get_int(before, "raw", 0);
    ncl_json_free(before);
    params = ncl_json_new_object();
    bytes = ncl_json_new_array();
    if (params == NULL || bytes == NULL) {
        ncl_json_free(params);
        ncl_json_free(bytes);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < 4u; i++) {
        long long byte = (raw >> (8 * (3u - i))) & 0xFF;

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
    (void)ncl_json_obj_set_string(params, "item", "WRPARAM");
    (void)ncl_json_obj_set_int(params, "block", 0);
    (void)ncl_json_obj_set_int(params, "d", number);
    (void)ncl_json_obj_set_int(params, "e", number);
    (void)ncl_json_obj_set(params, "data", bytes);
    rc = ncl_focas_call(focas, "payload", params, &answer);
    ncl_json_free(params);
    ncl_json_free(answer);
    if (rc != NCL_OK) {
        return note(focas, "写参数", rc);
    }
    if (had_raw && was_raw != raw) {
        /* 值本来就不同：写后复核（比**原始整数**，不跟小数位/量纲纠缠），
         * 没变就是没写进去。 */
        rc = ncl_focas_parameter(focas, number, &after);
        if (rc == NCL_OK && ncl_json_obj_get_int(after, "raw", was_raw) != raw) {
            ncl_json_free(after);
            return note(focas, "写参数", NCL_ERR_UNAVAILABLE);
        }
        ncl_json_free(after);
    }
    return NCL_OK;
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
