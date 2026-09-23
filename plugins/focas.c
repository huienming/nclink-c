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

#include <stdio.h>
#include <string.h>

#include "nclink/clients/focas.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_json.h"

/* 文件处理最后一段的实现（定义在下面），open() 里要它们的地址。 */
static ncl_err focas_file_push(void *user, const char *name, const char *path,
                               char **reason);
static ncl_err focas_file_pull(void *user, const char *name, const char *path,
                               char **reason);
static ncl_err focas_file_remove(void *user, const char *name, char **reason);

/* ---------------------------------------------------------------- 连接 ---- */

/**
 * 把配置里的 parameters 交给 client。会话在第一次读的时候才建，所以机床没开机
 * 不影响设备程序启动：谁问值，谁拿到一条明确的"读不到"。
 */
static void *focas_open(const ncl_json *params, char **err)
{
    /* 注册进文件工具的那份要活得比这次 open 长，所以放静态：一个进程一台机床。 */
    static ncl_file_backend backend;
    ncl_focas_config config;
    ncl_focas *focas;

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
    focas = ncl_focas_open(&config, err);
    if (focas == NULL) {
        return NULL;
    }
    /* 文件处理的最后一段（adapter → 机床）：把 FOCAS 的程序上下行交给文件工具，
     * 设备的 `/CONTROLLER/FILE` 从此多走这一段（见 nclink/ncl_file.h）。 */
    memset(&backend, 0, sizeof(backend));
    backend.user = focas;
    backend.push = focas_file_push;
    backend.pull = focas_file_pull;
    backend.remove = focas_file_remove;
    (void)ncl_file_tool_set_backend(&backend);
    return focas;
}

static void focas_close(void *ctx)
{
    /* 最后一段随连接一起撤（文件工具退回"只到本地目录"）。 */
    (void)ncl_file_tool_set_backend(NULL);
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

/** 一次读几把刀：每个号 4 条往返（半径/长度 × 几何/磨损），别让人一口气点几百次。 */
#define FOCAS_TOOL_BATCH_MAX 16u

/**
 * `params.keys`：按标准是个数组，也认单个（1 / "1" 都行）。@p index 越界回 false。
 * （与新代那条同一个口径，见 plugins/syntec.c 的 syntec_param_key。）
 */
static bool focas_key_at(const ncl_json *keys, size_t index, long long *out)
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

/** `params.keys` 里有几个号（单个算一个，没有算零）。 */
static size_t focas_key_count(const ncl_json *keys)
{
    size_t count;

    if (keys == NULL) {
        return 0;
    }
    count = ncl_json_arr_len(keys);
    return count > 0 ? count : 1u;
}

/* --------------------------------------------------------------- PMC ---- */

/*
 * `/CONTROLLER/REGISTER@<族>`：**PMC** 的位与寄存器（FANUC 的 PLC 就叫 PMC，§11.21）。
 * 一个族一条点位，族写在 `arg` 上：
 *
 *   REGISTER@X   X（机床→PMC 输入位）  0i-D：0..127 **字节** → 1024 位
 *   REGISTER@Y   Y（PMC→机床 输出位）  同上
 *   REGISTER@G   G（PMC→CNC）          0..767 字节 → 6144 位
 *   REGISTER@F   F（CNC→PMC）          同上
 *   REGISTER@R   R（内部继电器）       0..7999 字节 → 64000 位
 *   REGISTER@K   K（保持继电器）       0..99 字节 → 800 位
 *   REGISTER@D   D（数据表，**字**）   0..7999
 *
 * 号一律是**该族自己的编号**：位族用"字节.位"摊平的位号（`X0.0` = 0、`X1.0` = 8），
 * D 用字号。取值形状是 LIST（答 `get_length` / `get_value`（`keys` 给号）/
 * `get_attributes`），与新代那几条寄存器点位同一个口径。
 *
 * 号段上限用的是 spec 上 **0i-D** 那一版的表；机器实际支持多少可以用
 * `pmc_rdpmcinfo`（0x8003）问（帧已抓，见 §11.21.4）—— 这一轮先用文档值。
 *
 * **只读这一轮**：PMC 写（`pmc_wrpmcrng`）帧还没核（§11.21.5），点位就不声明写。
 */
#define FOCAS_REGISTER_BATCH_MAX 64

/** 族的容量：位族按**字节**算、D 按**字**算。认不出回 0。 */
static unsigned focas_register_capacity(char family, bool *words)
{
    *words = false;
    switch (family) {
    case 'X':
    case 'Y':
        return 128u;
    case 'G':
    case 'F':
        return 768u;
    case 'R':
        return 8000u;
    case 'K':
        return 100u;
    case 'D':
        *words = true;
        return 8000u;
    default:
        return 0u;
    }
}

static ncl_err focas_register_table(void *ctx, const ncl_tool_point *self,
                                    ncl_operation op, const ncl_json *params,
                                    ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    const char *family_text = (const char *)self->arg;
    const ncl_json *keys = ncl_params_get(params, "keys");
    char family;
    bool words = false;
    unsigned units;
    size_t i;
    ncl_err rc;

    if (family_text == NULL || family_text[0] == '\0') {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "这条点位没带族（@X/@R…）");
    }
    family = family_text[0];
    if (family >= 'a' && family <= 'z') {
        family = (char)(family - 'a' + 'A');
    }
    units = focas_register_capacity(family, &words);
    if (units == 0) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                             "不认识的 PMC 族 %c（X/Y/G/F/R/K/D）", family);
    }
    switch (op) {
    case NCL_OP_GET_LENGTH: /* LIST：这一族有多少个（位族按位、D 按字） */
        return ncl_tool_reply_int(result, words ? (long long)units
                                                : (long long)units * 8);
    case NCL_OP_GET_ATTRIBUTES: {
        ncl_json *array = ncl_json_new_array();
        const char *const fields[][2] = {
            {"number", words ? "字号（0 起，就是 keys）" : "位号（字节 × 8 + 位，0 起）"},
            {"value", words ? "数据表的值（u16）" : "位（true/false）"},
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
        size_t n = focas_key_count(keys);
        long long limit = words ? (long long)units : (long long)units * 8;
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) { /* 空 keys：不猜，答空的 */
            *result = out;
            return NCL_OK;
        }
        if (n > FOCAS_REGISTER_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)FOCAS_REGISTER_BATCH_MAX);
        }
        /*
         * 一个号一次往返：这一族本机是"一次只回一个点"（§11.21.3），
         * 就算段读也要退化成一个一个来，所以这里干脆按号读。
         */
        for (i = 0; i < n; i++) {
            long long no = 0;
            char name[24];

            if (!focas_key_at(keys, i, &no) || no < 0 || no >= limit) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是 %c 族的号（0..%lld）",
                                     (unsigned)(i + 1), family, limit - 1);
            }
            snprintf(name, sizeof(name), "%lld", no);
            if (words) {
                ncl_json *one = NULL;
                long long value = 0;

                rc = ncl_focas_pmc_read(focas, family, no, 1, 1, &one);
                if (rc != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, rc, "%s",
                                         ncl_focas_last_error(focas));
                }
                (void)ncl_json_as_int(ncl_json_arr_get(one, 0), &value);
                ncl_json_free(one);
                if (ncl_json_obj_set_int(out, name, value) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            } else {
                bool on = false;

                rc = ncl_focas_pmc_bit(focas, family, no, &on);
                if (rc != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, rc, "%s",
                                         ncl_focas_last_error(focas));
                }
                if (ncl_json_obj_set_bool(out, name, on) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: {
        const ncl_json *value_param = ncl_params_get(params, "value");
        size_t n = focas_key_count(keys);
        long long limit = words ? (long long)units : (long long)units * 8;

        if (value_param == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "set_value 要带 value");
        }
        if (n > FOCAS_REGISTER_BATCH_MAX) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)FOCAS_REGISTER_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            long long no = 0;
            const ncl_json *one = ncl_json_arr_len(value_param) > 0
                                      ? ncl_json_arr_get(value_param, i)
                                      : value_param;

            if (!focas_key_at(keys, i, &no) || no < 0 || no >= limit) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是 %c 族的号（0..%lld）",
                                     (unsigned)(i + 1), family, limit - 1);
            }
            if (one == NULL) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "value 比 keys 少（第 %u 个没有）",
                                     (unsigned)(i + 1));
            }
            if (words) {
                long long v = 0;
                ncl_err wrc;

                if (!ncl_json_as_int(one, &v)) {
                    return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                         "value 第 %u 个不是数", (unsigned)(i + 1));
                }
                wrc = ncl_focas_pmc_write(focas, family, no, &v, 1, 1);
                if (wrc != NCL_OK) {
                    return ncl_tool_fail(reason, wrc, "%s",
                                         ncl_focas_last_error(focas));
                }
            } else {
                bool on = false;
                ncl_err wrc;

                if (ncl_json_type_of(one) == NCL_JSON_BOOL) {
                    (void)ncl_json_as_bool(one, &on);
                } else {
                    long long v = 0;

                    if (!ncl_json_as_int(one, &v)) {
                        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                             "value 第 %u 个不是 true/false",
                                             (unsigned)(i + 1));
                    }
                    on = v != 0;
                }
                wrc = ncl_focas_pmc_bit_write(focas, family, no, on);
                if (wrc != NCL_OK) {
                    return ncl_tool_fail(reason, wrc, "%s",
                                         ncl_focas_last_error(focas));
                }
            }
        }
        *result = ncl_json_new_object();
        return *result != NULL ? NCL_OK : ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "PMC 寄存器表答 get_length / get_value / set_value / "
                             "get_attributes");
    }
}

/**
 * `/CONTROLLER/TOOL`：刀具表（表 7 的 TOOL，list）。**刀补号即 key（从 1 起）**，
 * 元素就是那把刀的 TOOLPARAM —— 与 10 册新代那条同口径（"一把刀就是一个元素、
 * TOOLPARAM 是 TOOL 的元素"）：
 *
 *     {"id":1,"kind":0,"radius":0.008,"length":0.0,
 *      "radius_wear":32.769,"length_wear":0.0}
 *
 * 读：`cnc_rdtofsinfo`（0x0a）问号上限、每个号 4 条 `cnc_rdtofs`（0x08）。
 * 写：`cnc_wrtofs`（0x09）—— 2026-09 对 VM 里的 0i-MF 模拟器**写进去又读回来**核过
 * （写 0.1234 → 读回 0.123，01 册 §11.13）。一次只写一个号，**只覆盖给到的字段**
 * （每个字段一条 0x09，别的字段不动）；权限在适配器外面控（用户口径：只提供能力）。
 *
 * `kind`（刀尖号）这条路上没有来源（`cnc_rdtooldata` 这台机床回 EW_FUNC），固定 0；
 * `time_usage`（寿命）也没有（`cnc_rdlife` 回 EW_NOOPT），get_attributes 里写明。
 */
static ncl_err focas_tool_table(void *ctx, const ncl_tool_point *self,
                                ncl_operation op, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    size_t i;
    ncl_err rc;

    (void)self;
    switch (op) {
    case NCL_OP_GET_LENGTH: {
        long long limit = 0;

        /* **机床自己报的号上限**（这台 400），不是"实际有几把刀" —— 后者要逐号扫
         * 一遍才知道，函数名字里写清楚了。 */
        rc = ncl_focas_tool_offset_count(focas, &limit);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
        }
        return ncl_tool_reply_int(result, limit);
    }
    case NCL_OP_GET_ATTRIBUTES: {
        static const char *const k_fields[][2] = {
            {"id", "刀补号（就是 key，从 1 起）"},
            {"kind", "刀尖号：这条路上没有来源，固定 0"},
            {"radius", "半径几何（cnc_rdtofs type 1）"},
            {"length", "长度几何（cnc_rdtofs type 3）"},
            {"radius_wear", "半径磨损（type 0）"},
            {"length_wear", "长度磨损（type 2）"},
            {"time_usage", "寿命：cnc_rdlife 回 EW_NOOPT（没开寿命管理选项），不给"},
        };
        ncl_json *array = ncl_json_new_array();

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
            ncl_json *entry = ncl_json_new_object();

            if (entry == NULL ||
                ncl_json_obj_set_string(entry, "name", k_fields[i][0]) != NCL_OK ||
                ncl_json_obj_set_string(entry, "meaning",
                                        k_fields[i][1]) != NCL_OK ||
                ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(array);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE: { /* 按刀补号取值（每个号 4 条 0x08） */
        size_t n = focas_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) { /* 盲读（轮询/自检）：答空的，不报错 */
            *result = out;
            return NCL_OK;
        }
        if (n > FOCAS_TOOL_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 把刀",
                                 (unsigned)FOCAS_TOOL_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            ncl_json *entry = NULL;
            long long no = 0;
            char name[16];

            if (!focas_key_at(keys, i, &no) || no < 1) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 里第 %u 个不是刀补号",
                                     (unsigned)(i + 1));
            }
            rc = ncl_focas_tool_param(focas, no, &entry);
            if (rc != NCL_OK) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, rc, "%s",
                                     ncl_focas_last_error(focas));
            }
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
    case NCL_OP_SET_VALUE: { /* 写刀补（0x09）；权限在适配器外面控 */
        const ncl_json *value = ncl_params_get(params, "value");
        long long no = 0;
        ncl_json *entry = NULL;
        ncl_json *reply;
        char name[16];

        if (!focas_key_at(keys, 0, &no) || no < 1) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=一个刀补号（从 1 起）");
        }
        if (value == NULL ||
            ncl_json_type_of((ncl_json *)value) != NCL_JSON_OBJECT) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 value=一条刀具参数对象");
        }
        /*
         * **只写给到的字段**（每个字段一条 0x09）：想改一个半径磨损就只给
         * `radius_wear`，别的字段不动 —— 与新代那条（先读回打底再覆盖）同一个口径，
         * 但这里不用打底：FOCAS 的写本来就是"一个字段一条命令"。
         */
        rc = ncl_focas_tool_param_write(focas, no, value);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
        }
        /* 答写完之后**机床里**的那一条（不是"我以为写进去的值"）。 */
        rc = ncl_focas_tool_param(focas, no, &entry);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
        }
        reply = ncl_json_new_object();
        if (reply == NULL) {
            ncl_json_free(entry);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        snprintf(name, sizeof(name), "%d", (int)no);
        if (ncl_json_obj_set(reply, name, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(reply);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = reply;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "刀具表答 get_length / get_value / set_value / get_attributes");
    }
}

/* ------------------------------------------------------ 文件处理的最后一段 -- */

/*
 * client → adapter → 机床 这条链的**最后一段**：文件在设备本地落地之后，由这里用
 * FOCAS 把它送进机床（`cnc_dwnstart4` 三件套），或从机床取回来（`cnc_upstart4`
 * 三件套）、删掉机床上的那份。注册进文件工具（`ncl_file_tool_set_backend()`），
 * 于是 `/CONTROLLER/FILE` 的 write / read / delete 就多走这一段（见 ncl_file.h）。
 *
 * 帧与体长按官方 SDK 实测（01 册 §2.4）；上行（取回）的应答切法还没核，所以
 * pull 现在会明确回"还读不了"（NCL_ERR_UNAVAILABLE）。
 */
static ncl_err focas_file_push(void *user, const char *name, const char *path,
                               char **reason)
{
    ncl_focas *focas = (ncl_focas *)user;
    char *bytes = NULL;
    ncl_err rc;

    rc = ncl_file_read_all(path, &bytes, NULL);
    if (rc != NCL_OK) {
        if (reason != NULL) {
            *reason = ncl_strdup("读本地文件失败");
        }
        return rc;
    }
    rc = ncl_focas_program_download(focas, 0, name, bytes);
    ncl_free_safe(bytes);
    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_focas_last_error(focas));
    }
    return rc;
}

static ncl_err focas_file_pull(void *user, const char *name, const char *path,
                               char **reason)
{
    ncl_focas *focas = (ncl_focas *)user;
    char *bytes = NULL;
    size_t len = 0;
    ncl_err rc = ncl_focas_program_upload(focas, 0, name, &bytes, &len);

    if (rc != NCL_OK) {
        if (reason != NULL) {
            *reason = ncl_strdup(ncl_focas_last_error(focas));
        }
        return rc;
    }
    /*
     * 取回来的字节要**落到本地** @p path 上 —— 设备侧的 `/CONTROLLER/FILE` 的 `pull`
     * 就是"从机床取回这个文件"，不写盘等于什么都没做（原来这里 `(void)path;` 把它丢了，
     * 2026-09-22 按 10 册（新代）那条的写法修好）。
     */
    rc = ncl_file_write_all(path, bytes, len);
    ncl_free_safe(bytes);
    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup("写本地文件失败");
    }
    return rc;
}

static ncl_err focas_file_remove(void *user, const char *name, char **reason)
{
    ncl_focas *focas = (ncl_focas *)user;
    ncl_err rc = ncl_focas_program_delete(focas, name);

    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_focas_last_error(focas));
    }
    return rc;
}

/* ------------------------------------------------------------------ 工具 -- */

/**
 * PMC 寄存器这一族支持的操作用掩码：读的三条（这一轮不开写）。
 */
#define FOCAS_REGISTER_OPS                                                     \
    (NCL_OP_BIT(NCL_OP_GET_LENGTH) | NCL_OP_BIT(NCL_OP_GET_VALUE) |           \
     NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))
/** 能写的族再带上 `set_value`（`X`/`F` 是机床/CNC 驱动的信号，只读）。 */
#define FOCAS_REGISTER_RW_OPS (FOCAS_REGISTER_OPS | NCL_OP_BIT(NCL_OP_SET_VALUE))

NCL_TOOL_BEGIN("focas", "FANUC FOCAS / Fwlib32 over TCP（读为主，刀补表可写）", "MACHINE", 1000, 1000,
               focas_open, focas_close)

    /* 默认采样通道只放四样（现场口径）：设备状态、加工计件、程序名称、报警。 */
    NCL_DATAITEM_STR_SAMPLED("/STATUS", ncl_focas_status)
    /* 工作模式（表 7 的 WORK_MODE，取值 manual / auto，表 8）：同一个 STATINFO
     * 位域推出来，不多花报文，从默认采样通道读。 */
    NCL_DATAITEM_STR("/WORK_MODE", ncl_focas_mode)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", ncl_focas_part_count)
    /* 表 6/表 7 的对象元信息（进 configs，不进采样通道）：厂商这一条不用读机床；
     * 型号与版本来自会话探针那条 `code 24` 的 ODBSYS（真机实测，01 册 §2.8）——
     * 本机出门是 "0M D4G3" / "28.0"。 */
    NCL_CONFIG_STR("/MANUFACTURER", ncl_focas_manufacturer)
    NCL_CONFIG_STR("/MODEL", ncl_focas_model)
    NCL_CONFIG_STR("/VERSION", ncl_focas_version)
    /* PROGRAM / PROGRAM_NUMBER / LINE_NUMBER 属 CONTROLLER 组件（SUBPROGRAM 不要了：
 * 官方库对 cnc_rdexecprog3 一帧都不发，见 01 册 §11.19.4）
     * （表 2：组件对象）。前三条是真读的（EXEPRGNAME2 / cnc_rdprgnum /
     * cnc_rdseqnum），子程序号要 cnc_rdexecprog3（帧待抓）。 */
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_focas_program_name)
    NCL_DATAITEM_I64("/CONTROLLER/PROGRAM_NUMBER", ncl_focas_program_number)
    NCL_DATAITEM_STR("/LINE_NUMBER", ncl_focas_line_number)
    /* 当前刀号（表 7 的 TOOL_NUMBER）：`cnc_rdcommand`（0x97）指令值里 `adrs='T'`
     * 那条的 `cmd_val`（2026-09-23 抓帧，01 册 §11.19.2）。
     * 倍率都在操作面板信号（0x5d）里：进给倍率 @0xa、主轴倍率 @0xc（就在它后面一格），
     * 码值 0..20 = 0%..200%。**这台机器的 0x5d 载荷是桩**（32 字节没填），所以两个
     * 倍率在这台机器上都读不到真值 —— 进给那条落进码表看着像 0%，主轴那条直接报错。 */
    NCL_DATAITEM_I64("/TOOL_NUMBER", ncl_focas_tool_number)
    NCL_DATAITEM_F64("/FEED_OVERRIDE", ncl_focas_feed_override)
    NCL_DATAITEM_F64("/SPINDLE_OVERRIDE", ncl_focas_spindle_override)
    NCL_DATAITEM_F64("/FEED_SPEED", ncl_focas_feed_speed)

    /* 报警（表 6 的 WARNING）：进默认采样通道。真机实测（01 册 §2.8.2）：没报警时
     * 机床回 0 字节载荷 → 这里给**空数组**；有报警时给 `{"number":75,"type":3,
     * "text":"保护"}`（机床的文本是 GB2312，client 出门前过 ncl_gb2312_to_utf8）。
     * 注意 `0x23` 那两个暗格（arg2=2 / arg3=64）不填就永远拿不到文本。 */
    NCL_DATAITEM_JSON_SAMPLED("/WARNING", ncl_focas_alarm)

    /* 五轴的位置与进给速度。线性轴的位置是 POSITION（mm），旋转轴（A/C）是
     * ANGLE（角度）。位置这一类都是**真读**的（真机实测，01 册 §2.8.1）：
     *   @REAL  实际位置 —— cnc_rdposition（一条 8 块，下标 1 = 绝对），每轴一条
     *          **8 字节记录**：data(BE32)@0 + dec(BE16)@6，值 = data / 10^dec；
     *   @CMD   指令位置 —— 现场口径"跟踪误差 = 实际 − 指令"，所以指令 = 实际 −
     *          cnc_srvdelay（0x26 d=9）；机床静止时两条相等。
     * 进给速度也是真读的 —— cnc_actf（0x24，每轴一条 8 字节记录），mm/min。
     * 名字从路径自动推：/MACHINE/AXIS@X/POSITION@REAL -> AXIS_X.POSITION_REAL。 */
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
    /* 旋转轴的位置按表 4 的 ANGLE 报（同一根轴、同一个读法）。 */
    NCL_DATAITEM_F64("/AXIS@A/ANGLE@REAL", ncl_focas_axis_position,
                     NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/ANGLE@REAL", ncl_focas_axis_position,
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

    NCL_DATAITEM_F64("/AXIS@X/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_C)
    /* 轴的剩余进给（表 7 的 PATH_LEFT_LENGTH）、扭矩/电流/温度（表 4）与轴类型
     * （表 7 的 TYPE，进 configs）：请求码/来源都写在 client 的注释里。 */
    NCL_DATAITEM_F64("/AXIS@X/PATH_LEFT_LENGTH", ncl_focas_axis_distance,
                     NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@X/TORQUE", ncl_focas_axis_torque, NCL_FOCAS_AXIS_X)
    /* 轴电流（安培）= cnc_rdsvmeter 0x56 的 d=3（d=1 那格是负载表 %）。 */
    NCL_DATAITEM_F64("/AXIS@X/CURRENT", ncl_focas_axis_current, NCL_FOCAS_AXIS_X)
    /*
     * **没有轴温这个点位列**：FOCAS 里没有读轴温的调用（只有智能终端的高温报警码），
     * 与其挂一个永远回"待抓包"的格子，不如不摆（表 4 的点位也不是必须全摆）。
     */
    NCL_CONFIG_STR("/AXIS@X/TYPE", ncl_focas_axis_type, NCL_FOCAS_AXIS_X)
    /* 主轴：表 2 的组件类型里没有 SPINDLE，最接近的是 MOTOR（主轴就是主轴电机
     * 驱动的），所以主轴转速按 SPEED（units rpm）报在 /MOTOR@S1 下；负载（%）
     * 字典里没有对应项，只留在 client 的 API 里。 */
    NCL_DATAITEM_F64("/MOTOR@S1/SPEED", ncl_focas_spindle_speed, 0)

    /* 刀具表（表 7 的 TOOL，list）：元素就是那把刀的 TOOLPARAM（见 focas_tool_table
     * 的说明）。**读 + 写**都开：读是 0x0a 定号上限 + 每号 4 条 0x08，写是 0x09
     * （2026-09 对模拟器写进去又读回来核过，01 册 §11.13）。写权限在适配器外面控
     * —— 用户口径是"只提供能力"。 */
    NCL_CONFIG_OPS("/CONTROLLER/TOOL", focas_tool_table, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))
    /* 刀具参数（表 7 的 TOOLPARAM，JSON 对象）：**号 → 那一条**（与新代那条不同，
     * 这里两种摆法都留着：TOOL 是 list（按号取元素），TOOLPARAM 是整张表）。 */
    NCL_CONFIG_JSON("/CONTROLLER/TOOLPARAM", ncl_focas_tool_param_table)
    /* 参数表（表 6 的 PARAMETER，dict）与宏变量表（表 7 的 VARIABLE，list）：
     * **单条**读得到（`cnc_rdparam` 0x8d / `cnc_rdmacro` 0x15，见 client），整表的
     * 范围调用（rdparanum/rdparar/rdmacror）在这台机器上被拒（§2.8.6）——哪天有机器
     * 支持整表，范围从 rdtofsinfo/rdmacroinfo 拿（§2.8.7）。 */
    NCL_CONFIG_JSON("/CONTROLLER/PARAMETER", ncl_focas_parameter_table)
    NCL_CONFIG_JSON("/CONTROLLER/VARIABLE", ncl_focas_variable_table)
    /* 工件坐标系（表 7 的 COORDINATE，JSON 对象 → 表 9 的 x/y/z…）：
     * `cnc_rdwkcdshft` type 0..20 全试过，这台机器一律 rc=1 —— 机床不提供。 */
    NCL_CONFIG_JSON("/CONTROLLER/COORDINATE", ncl_focas_work_offsets)

    /*
     * PMC / 寄存器（`/CONTROLLER/REGISTER@<族>`）：位族答位（true/false），
     * `@D` 答字。号是各族自己的编号（位族按"字节 × 8 + 位"）。只读这一轮。
     */
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@X", focas_register_table, "X",
                   FOCAS_REGISTER_OPS) /* X = 机床驱动的输入：只读 */
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@Y", focas_register_table, "Y",
                   FOCAS_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@G", focas_register_table, "G",
                   FOCAS_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@F", focas_register_table, "F",
                   FOCAS_REGISTER_OPS) /* F = CNC 驱动的：只读 */
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@R", focas_register_table, "R",
                   FOCAS_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@K", focas_register_table, "K",
                   FOCAS_REGISTER_RW_OPS)
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@D", focas_register_table, "D",
                   FOCAS_REGISTER_RW_OPS)

    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_CALL("/SESSION", session_method)
    NCL_METHOD_CALL("/ITEMS", items_method)

    /*
     * **文件/程序不在这里声明。** 文件的门面只有一个：文件工具声明的
     * `/CONTROLLER/FILE`（dict，进 configs），它的操作走标准那一套（`get_value` /
     * `get_attributes` / `get_keys` / `add` / `delete` / `call`），**传输的最后
     * 一段（adapter → 机床）**才由这份工具的 client 用 FOCAS 去搬
     * （`cnc_dwnstart4` 三件套一族 / `cnc_upstart4` 一族，见 clients/focas）。
     * 给 FOCAS 工具另开 `/PROGRAM@DOWNLOAD` 这种方法，等于把"文件"做成两套流程
     * —— 现场那家的门面是 `/CONTROLLER/{CONSOLE,FILE,PROGRAM_DATA}`，没有一条
     * 挂在设备节点上。
     */

NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.5.1", "FANUC FOCAS / Fwlib32 over TCP（读为主 + 刀补表可写；会话与报文形状按真机/模拟器实测：01 册 §2.8/§11.13；点位对照 32 册数据项）")
