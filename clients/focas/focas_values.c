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
 * 状态：读 STATINFO 的前十个 int16（= ODBST 的前 20 字节），由 RUN（载荷偏移 2）
 * 与 EMERGENCY（偏移 10）两位决定三态。三次读并成一次，报文不多花。
 */
ncl_err ncl_focas_status(ncl_focas *focas, char *out, size_t cap)
{
    ncl_json *bits = NULL;
    long long running = 0;
    long long holding = 0;
    ncl_err rc;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "STATINFO@0", 0, 10, NCL_DTYPE_INT16, &bits);
    if (rc != NCL_OK) {
        return rc;
    }
    (void)ncl_json_as_int(ncl_json_arr_get(bits, 1), &running);  /* STATINFO@2  */
    (void)ncl_json_as_int(ncl_json_arr_get(bits, 5), &holding);  /* STATINFO@10 */
    ncl_json_free(bits);
    snprintf(out, cap, "%s",
             holding != 0 ? "holding" : running != 0 ? "running" : "free");
    return NCL_OK;
}

/* RDCOUNT 是 int32，直接交数值（表 7 的 PART_COUNT 是 number）。 */
ncl_err ncl_focas_part_count(ncl_focas *focas, long long *value)
{
    ncl_json *json = NULL;
    ncl_err rc;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_focas_read_item(focas, "RDCOUNT", 0, 1, NCL_DTYPE_INT32, &json);
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

/** 两根轴共用的取值：位置是 ACTF、速度是 ACTS，都是每轴 4 字节的 float。 */
static ncl_err axis_value(ncl_focas *focas, ncl_focas_axis axis,
                          const char *item_name, double *value)
{
    char item[32];
    ncl_json *json = NULL;
    ncl_err rc;

    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, item_name, NCL_ERR_RANGE);
    }
    snprintf(item, sizeof(item), "%s@%d", item_name, (int)axis * 4);
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

ncl_err ncl_focas_axis_position(ncl_focas *focas, ncl_focas_axis axis,
                                double *value)
{
    return axis_value(focas, axis, "ACTF", value);
}

ncl_err ncl_focas_axis_speed(ncl_focas *focas, ncl_focas_axis axis,
                             double *value)
{
    return axis_value(focas, axis, "ACTS", value);
}

/* ------------------------------------------------------- 还没抓到帧的调用 -- */

/*
 * 现场要的三个量，协议调用还没抓到帧（01 册 §2.3 的码表里没有 cnc_rdalmmsg2；位置
 * 与刀具表的帧还没核对）。它们先按现场要的样子摆在这里，回 NCL_ERR_UNAVAILABLE：
 * 适配器照常把这三个函数绑到模型路径上，于是那些点位在模型里看得见、问它答"还读
 * 不了"、轮询与 §6 审计都不碰、自检算成"待抓包"而不是失败。
 *
 * 为什么"没实现"也要写在 client 里：帧格式、item 名、要读哪几块是这一层的知识。
 * 在适配器里再发明一套"待抓包"的写法，除了重复一句理由什么也表达不了，而且抓包
 * 补上时得改两处（点位表 + client）；现在只改下面的函数体，点位表一行都不用动。
 */

/** 记一句"要抓哪一帧"再回 NCL_ERR_UNAVAILABLE（last_error 里排障看得到）。 */
static ncl_err not_yet(ncl_focas *focas, const char *what, const char *call)
{
    if (focas != NULL) {
        snprintf(focas->error, sizeof(focas->error), "%s：帧待抓包（%s）", what,
                 call);
    }
    return NCL_ERR_UNAVAILABLE;
}

ncl_err ncl_focas_alarm(ncl_focas *focas, ncl_json **value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *value = NULL;
    /* 帧补上之后：cnc_rdalmmsg2 读报警表，回 {"number","text"}（多条报警就是数组）。 */
    return not_yet(focas, "报警", "cnc_rdalmmsg2，01 册 §2.3 / 31 册");
}

ncl_err ncl_focas_axis_position_cmd(ncl_focas *focas, ncl_focas_axis axis,
                                    double *value)
{
    if (focas == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if ((int)axis < 0 || (int)axis >= (int)NCL_FOCAS_AXIS_COUNT) {
        return note(focas, "AXIS", NCL_ERR_RANGE);
    }
    /* 帧补上之后：位置那一路（cnc_rdposition）一次给两列（实际/目标），目标位置
     * 就是它多出来的那一列 —— 和 ncl_focas_axis_position() 同一个读法。 */
    return not_yet(focas, "目标位置", "cnc_rdposition");
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
