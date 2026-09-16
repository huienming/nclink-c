/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * C# 绑定的原生垫片（flat C ABI）。
 *
 * 为什么不直接 P/Invoke 库本身：
 *   1. 库里没有导出宏（静态库，没有 __declspec(dllexport)），Windows 上没法直接
 *      DllImport；
 *   2. 托管侧不该依赖 C 结构体的内存布局（ncl_message / ncl_sample_item /
 *      ncl_node ...），库一改字段就会静默错位。
 *
 * 所以这里只暴露三种东西：不透明句柄（void*）、标量、JSON 文本。
 *
 * 内存约定：
 *   - 返回 char* 的，缓冲区是 malloc 出来的，调用方用 nclshim_free() 释放；
 *   - 返回 const char* / const void* 的，都是**借用**指针，句柄一释放就失效；
 *   - 回调里拿到的 message 句柄只在回调期间有效，要留就自己复制成 JSON 文本。
 */

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"

#if defined(_WIN32)
#define NCLSHIM_API __declspec(dllexport)
#else
#define NCLSHIM_API __attribute__((visibility("default")))
#endif

/* ----------------------------------------------------------------- misc -- */

/** 库版本号（编译期常量，不用释放）。 */
NCLSHIM_API const char *nclshim_version(void)
{
    return NCL_VERSION;
}

/** 错误码文本。 */
NCLSHIM_API const char *nclshim_err_name(int code)
{
    return ncl_err_name((ncl_err)code);
}

/** 释放垫片返还的所有权内存（char* / JSON / 模型）。 */
NCLSHIM_API void nclshim_free(void *ptr)
{
    free(ptr);
}

/** 安装根目录（conf/、bin/、log/ 都在它下面）。 */
NCLSHIM_API void nclshim_env_set_root(const char *root)
{
    ncl_env_set_root(root);
}

NCLSHIM_API const char *nclshim_env_root(void)
{
    return ncl_env_root();
}

/** 日志：NULL/空目录 = 默认的 <root>/log。返回 1 成功。 */
NCLSHIM_API int nclshim_log_init(const char *dir)
{
    return ncl_log_init(dir != NULL && dir[0] != '\0' ? dir : NULL) ? 1 : 0;
}

NCLSHIM_API void nclshim_log_shutdown(void)
{
    ncl_log_shutdown();
}

/** 0=Debug 1=Info 2=Warn 3=Error 4=Fatal（见 ncl_log_level）。 */
NCLSHIM_API void nclshim_log_set_level(int level)
{
    ncl_log_set_level((ncl_log_level)level);
}

NCLSHIM_API void nclshim_log_set_console(int enabled)
{
    ncl_log_set_console(enabled != 0);
}

/* ----------------------------------------------------------------- json -- */

/** 解析 JSON 文本；失败返回 NULL（不抛异常）。 */
NCLSHIM_API const void *nclshim_json_parse(const char *text)
{
    return text != NULL ? ncl_json_parse_cstr(text, NULL) : NULL;
}

NCLSHIM_API const void *nclshim_json_clone(const void *json)
{
    return json != NULL ? ncl_json_clone((const ncl_json *)json) : NULL;
}

NCLSHIM_API void nclshim_json_free(const void *json)
{
    ncl_json_free((ncl_json *)json);
}

/** 紧凑 JSON 文本（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_json_write(const void *json)
{
    return json != NULL ? ncl_json_write_string((const ncl_json *)json) : NULL;
}

/** ncl_json_type：0 null / 1 bool / 2 number / 3 string / 4 array / 5 object。 */
NCLSHIM_API int nclshim_json_type(const void *json)
{
    return json != NULL ? (int)ncl_json_type_of((const ncl_json *)json) : 0;
}

NCLSHIM_API int nclshim_json_is_null(const void *json)
{
    return ncl_json_is_null((const ncl_json *)json) ? 1 : 0;
}

NCLSHIM_API int nclshim_json_as_int(const void *json, long long *out)
{
    return json != NULL && ncl_json_as_int((const ncl_json *)json, out) ? 1 : 0;
}

NCLSHIM_API int nclshim_json_as_double(const void *json, double *out)
{
    return json != NULL && ncl_json_as_double((const ncl_json *)json, out) ? 1 : 0;
}

NCLSHIM_API int nclshim_json_as_bool(const void *json, int *out)
{
    bool value = false;

    if (json == NULL || !ncl_json_as_bool((const ncl_json *)json, &value)) {
        return 0;
    }
    *out = value ? 1 : 0;
    return 1;
}

/** 字符串值（借用；不是字符串返回 NULL）。 */
NCLSHIM_API const char *nclshim_json_string(const void *json)
{
    return json != NULL ? ncl_json_as_string((const ncl_json *)json) : NULL;
}

/** 文本形式：数字原样、字符串去引号（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_json_text(const void *json)
{
    return json != NULL ? ncl_json_as_text((const ncl_json *)json) : NULL;
}

/** 数组元素个数 / 对象成员个数。 */
NCLSHIM_API int nclshim_json_count(const void *json)
{
    if (json == NULL) {
        return 0;
    }
    return ncl_json_type_of((const ncl_json *)json) == NCL_JSON_ARRAY
               ? (int)ncl_json_arr_len((const ncl_json *)json)
               : (int)ncl_json_obj_len((const ncl_json *)json);
}

NCLSHIM_API const void *nclshim_json_array_get(const void *json, int index)
{
    return json != NULL ? ncl_json_arr_get((const ncl_json *)json, (size_t)index)
                        : NULL;
}

NCLSHIM_API const void *nclshim_json_object_value_at(const void *json, int index)
{
    return json != NULL ? ncl_json_obj_val_at((const ncl_json *)json, (size_t)index)
                        : NULL;
}

NCLSHIM_API const char *nclshim_json_object_key_at(const void *json, int index)
{
    return json != NULL ? ncl_json_obj_key_at((const ncl_json *)json, (size_t)index)
                        : NULL;
}

NCLSHIM_API const void *nclshim_json_object_get(const void *json, const char *key)
{
    return json != NULL ? ncl_json_obj_get((const ncl_json *)json, key) : NULL;
}

/* ---------------------------------------------------------------- model -- */

/** 解析模型文档（NULL 或空串 = 库内置默认模型）。调用方用 nclshim_model_free。 */
NCLSHIM_API const void *nclshim_model_parse(const char *json)
{
    return ncl_root_node_parse(json != NULL && json[0] != '\0' ? json : NULL);
}

NCLSHIM_API void nclshim_model_free(const void *root)
{
    ncl_node_free((ncl_node *)root);
}

/** 序列化整棵树（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_model_write(const void *root)
{
    return root != NULL ? ncl_node_write_string((const ncl_node *)root) : NULL;
}

NCLSHIM_API const void *nclshim_model_find_by_id(const void *root, const char *id)
{
    return root != NULL ? ncl_node_find_by_id((const ncl_node *)root, id) : NULL;
}

/* 节点字段：全是借用指针，树释放即失效。 */

NCLSHIM_API int nclshim_node_type(const void *node)
{
    return node != NULL ? (int)((const ncl_node *)node)->type : 0;
}

NCLSHIM_API const char *nclshim_node_type_name(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->node_type_name : NULL;
}

NCLSHIM_API const char *nclshim_node_name(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->name : NULL;
}

NCLSHIM_API const char *nclshim_node_id(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->id : NULL;
}

NCLSHIM_API const char *nclshim_node_path(const void *node)
{
    return node != NULL ? ncl_node_path((const ncl_node *)node) : NULL;
}

NCLSHIM_API const char *nclshim_node_description(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->description : NULL;
}

NCLSHIM_API const char *nclshim_node_number(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->number : NULL;
}

NCLSHIM_API const char *nclshim_node_data_type(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->data_type : NULL;
}

NCLSHIM_API const char *nclshim_node_value_type(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->value_type : NULL;
}

NCLSHIM_API const char *nclshim_node_mapping(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->mapping : NULL;
}

NCLSHIM_API const char *nclshim_node_source(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->source : NULL;
}

NCLSHIM_API const char *nclshim_node_version(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->version : NULL;
}

NCLSHIM_API const char *nclshim_node_guid(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->guid : NULL;
}

NCLSHIM_API int nclshim_node_settable(const void *node)
{
    return node != NULL && ((const ncl_node *)node)->settable ? 1 : 0;
}

/** 数据项的当前值（借用；模型里没有值就是 NULL）。 */
NCLSHIM_API const void *nclshim_node_value(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->value : NULL;
}

/** 采样通道：周期（毫秒）与采样项。 */
NCLSHIM_API int nclshim_node_is_sample_channel(const void *node)
{
    return node != NULL && ncl_node_is_sample_node((const ncl_node *)node) ? 1 : 0;
}

NCLSHIM_API long long nclshim_node_sample_interval(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->sample_interval : 0;
}

NCLSHIM_API long long nclshim_node_upload_interval(const void *node)
{
    return node != NULL ? ((const ncl_node *)node)->upload_interval : 0;
}

NCLSHIM_API int nclshim_node_sample_item_count(const void *node)
{
    return node != NULL ? (int)ncl_node_sample_count((const ncl_node *)node) : 0;
}

/** 采样项声明的路径（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_node_sample_item_path(const void *node, int index)
{
    ncl_sample_ref *ref =
        node != NULL ? ncl_node_sample_at((const ncl_node *)node, (size_t)index) : NULL;

    return ref != NULL ? ncl_sample_ref_path(ref) : NULL;
}

/** 子节点遍历：kind 1=device 2=component 3=config 0=dataItem。 */
NCLSHIM_API int nclshim_node_count(const void *node, int kind)
{
    const ncl_node *n = (const ncl_node *)node;

    if (n == NULL) {
        return 0;
    }
    /* 库只给了"全部子节点"的计数，按类别数就把对应的 vector 长度读出来。 */
    switch (kind) {
    case 1: return (int)ncl_ptrvec_len(&n->devices);
    case 2: return (int)ncl_ptrvec_len(&n->components);
    case 3: return (int)ncl_ptrvec_len(&n->configs);
    default: return (int)ncl_ptrvec_len(&n->data_items);
    }
}

NCLSHIM_API const void *nclshim_node_at(const void *node, int kind, int index)
{
    const ncl_node *n = (const ncl_node *)node;

    if (n == NULL) {
        return NULL;
    }
    switch (kind) {
    case 1: return ncl_node_device_at(n, (size_t)index);
    case 2: return ncl_node_component_at(n, (size_t)index);
    case 3: return ncl_node_config_at(n, (size_t)index);
    default: return ncl_node_data_item_at(n, (size_t)index);
    }
}

/* ------------------------------------------------------- message / sample -- */

/** 整条报文的 JSON 文本（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_message_write(const void *msg)
{
    return msg != NULL ? ncl_message_write_string((const ncl_message *)msg) : NULL;
}

NCLSHIM_API int nclshim_message_type(const void *msg)
{
    return msg != NULL ? (int)((const ncl_message *)msg)->type : -1;
}

/** 采样报文：行数 = 数据最多的那一列的点数。 */
NCLSHIM_API int nclshim_sample_rows(const void *msg)
{
    return msg != NULL ? (int)ncl_message_sample_point_count((const ncl_message *)msg)
                       : 0;
}

/** 采样报文：列数（= 表头项数）。 */
NCLSHIM_API int nclshim_sample_columns(const void *msg)
{
    return msg != NULL ? (int)ncl_message_item_count((const ncl_message *)msg) : 0;
}

NCLSHIM_API int nclshim_sample_is_complete(const void *msg)
{
    return msg != NULL && ncl_message_sample_is_complete((const ncl_message *)msg) ? 1
                                                                                  : 0;
}

NCLSHIM_API const char *nclshim_sample_id(const void *msg)
{
    return msg != NULL ? ((const ncl_message *)msg)->as.sample.id : NULL;
}

NCLSHIM_API const char *nclshim_sample_begin_time(const void *msg)
{
    return msg != NULL ? ((const ncl_message *)msg)->as.sample.begin_time : NULL;
}

NCLSHIM_API long long nclshim_sample_interval(const void *msg)
{
    return msg != NULL ? ((const ncl_message *)msg)->as.sample.interval : 0;
}

NCLSHIM_API long long nclshim_sample_upload_interval(const void *msg)
{
    return msg != NULL ? ((const ncl_message *)msg)->as.sample.upload_interval : 0;
}

/** 表头第 col 项（借用）。 */
NCLSHIM_API const char *nclshim_sample_path(const void *msg, int col)
{
    const ncl_message *m = (const ncl_message *)msg;

    if (m == NULL || col < 0 || (size_t)col >= ncl_strvec_len(&m->as.sample.paths)) {
        return NULL;
    }
    return ncl_strvec_at(&m->as.sample.paths, (size_t)col);
}

/** 表头拼成一行（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_sample_header(const void *msg, const char *separator)
{
    return msg != NULL
               ? ncl_message_sample_header((const ncl_message *)msg, separator)
               : NULL;
}

/** 某列的形态：槽位数、总点数、是否批量列、编码。 */
NCLSHIM_API int nclshim_sample_column_slots(const void *msg, int col)
{
    const ncl_message *m = (const ncl_message *)msg;
    const ncl_sample_item *item =
        m != NULL ? (const ncl_sample_item *)ncl_message_item_at(m, (size_t)col) : NULL;

    return item != NULL && item->data != NULL ? (int)ncl_json_arr_len(item->data) : 0;
}

NCLSHIM_API int nclshim_sample_column_points(const void *msg, int col)
{
    const ncl_message *m = (const ncl_message *)msg;
    const ncl_sample_item *item =
        m != NULL ? (const ncl_sample_item *)ncl_message_item_at(m, (size_t)col) : NULL;

    return item != NULL ? (int)ncl_sample_item_value_count(item) : 0;
}

NCLSHIM_API int nclshim_sample_column_nested(const void *msg, int col)
{
    const ncl_message *m = (const ncl_message *)msg;
    const ncl_sample_item *item =
        m != NULL ? (const ncl_sample_item *)ncl_message_item_at(m, (size_t)col) : NULL;

    return ncl_sample_item_is_nested(item) ? 1 : 0;
}

NCLSHIM_API const char *nclshim_sample_column_encoding(const void *msg, int col)
{
    const ncl_message *m = (const ncl_message *)msg;
    const ncl_sample_item *item =
        m != NULL ? (const ncl_sample_item *)ncl_message_item_at(m, (size_t)col) : NULL;

    return item != NULL ? item->encoding : NULL;
}

/** 按行读：第 row 行、第 col 列（借用；越界返回 NULL）。 */
NCLSHIM_API const void *nclshim_sample_value_at(const void *msg, int row, int col)
{
    return msg != NULL ? ncl_message_sample_value_at((const ncl_message *)msg,
                                                     (size_t)row, (size_t)col)
                       : NULL;
}

/** 列内扁平取值（借用）：只按该列自己的顺序，不跨列对齐。 */
NCLSHIM_API const void *nclshim_sample_column_value_at(const void *msg, int col,
                                                       int index)
{
    const ncl_message *m = (const ncl_message *)msg;
    const ncl_sample_item *item =
        m != NULL ? (const ncl_sample_item *)ncl_message_item_at(m, (size_t)col) : NULL;

    return ncl_sample_item_value_at(item, (size_t)index);
}

/** 事件报文：id / time / key / value。 */
NCLSHIM_API const char *nclshim_event_id(const void *msg)
{
    return msg != NULL ? ((const ncl_message *)msg)->as.event.id : NULL;
}

NCLSHIM_API const char *nclshim_event_time(const void *msg)
{
    return msg != NULL ? ((const ncl_message *)msg)->as.event.time : NULL;
}

NCLSHIM_API const char *nclshim_event_key(const void *msg)
{
    const ncl_message *m = (const ncl_message *)msg;

    return m != NULL ? ncl_json_obj_get_string(m->as.event.event, "key") : NULL;
}

NCLSHIM_API const void *nclshim_event_value(const void *msg)
{
    const ncl_message *m = (const ncl_message *)msg;

    return m != NULL ? ncl_json_obj_get(m->as.event.event, "value") : NULL;
}

/* --------------------------------------------------------------- client -- */

typedef void (*nclshim_message_cb)(void *user, const char *topic, const void *msg);

/** 进程级初始化：连 broker（用户名/密码可为 NULL）。 */
NCLSHIM_API int nclshim_open(const char *uri, const char *user, const char *pass)
{
    return (int)ncl_client_holder_init(uri, user, pass);
}

/** 关掉连接与所有客户端。 */
NCLSHIM_API void nclshim_close(void)
{
    ncl_client_holder_shutdown();
}

NCLSHIM_API int nclshim_is_open(void)
{
    return ncl_client_holder_is_initialised() ? 1 : 0;
}

/** 取（必要时创建）某个 SN 的设备客户端；借用，nclshim_close 后失效。 */
NCLSHIM_API const void *nclshim_client_get(const char *sn)
{
    return ncl_client_holder_get(sn);
}

/** probe：拿模型。模型所有权交给调用方（nclshim_model_free）。 */
NCLSHIM_API int nclshim_client_probe(const void *client, unsigned timeout_ms,
                                     const void **out_model)
{
    ncl_message *response = NULL;
    ncl_node *model;
    ncl_err rc;

    if (out_model != NULL) {
        *out_model = NULL;
    }
    if (client == NULL || out_model == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_client_probe((ncl_client *)client, timeout_ms, &response);
    if (rc != NCL_OK || response == NULL) {
        ncl_message_free(response);
        return rc != NCL_OK ? (int)rc : NCL_ERR;
    }
    model = ncl_message_take_model(response);
    ncl_message_free(response);
    if (model == NULL) {
        return NCL_ERR_INVALID_MODEL;
    }
    *out_model = model;
    return NCL_OK;
}

/** getValue(path, timeout)：值所有权交给调用方（nclshim_json_free）。 */
NCLSHIM_API int nclshim_client_get_value(const void *client, const char *path,
                                         unsigned timeout_ms, const void **out_json)
{
    ncl_json *value = NULL;
    ncl_err rc;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (client == NULL || path == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_client_get_value((ncl_client *)client, path, timeout_ms, &value);
    if (rc != NCL_OK) {
        ncl_json_free(value);
        return (int)rc;
    }
    *out_json = value;
    return NCL_OK;
}

/** getValueRange(path, start, end, timeout)。 */
NCLSHIM_API int nclshim_client_get_value_range(const void *client, const char *path,
                                               int start, int end,
                                               unsigned timeout_ms,
                                               const void **out_json)
{
    ncl_json *value = NULL;
    ncl_err rc;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (client == NULL || path == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_client_get_value_range((ncl_client *)client, path, start, end, timeout_ms,
                                    &value);
    if (rc != NCL_OK) {
        ncl_json_free(value);
        return (int)rc;
    }
    *out_json = value;
    return NCL_OK;
}

/** getLength(path, timeout)。 */
NCLSHIM_API int nclshim_client_get_length(const void *client, const char *path,
                                          unsigned timeout_ms, long long *out_length)
{
    if (client == NULL || path == NULL || out_length == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return (int)ncl_client_get_length((ncl_client *)client, path, timeout_ms,
                                      out_length);
}

/** setValue(path, value, timeout)；value 是 JSON 文本。 */
NCLSHIM_API int nclshim_client_set_value(const void *client, const char *path,
                                         const char *value_json,
                                         unsigned timeout_ms)
{
    ncl_json *value;
    int rc;

    if (client == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    value = ncl_json_parse_cstr(value_json != NULL ? value_json : "null", NULL);
    if (value == NULL) {
        return NCL_ERR_PARSE;
    }
    rc = (int)ncl_client_set_value((ncl_client *)client, path, value, timeout_ms);
    if (rc != NCL_OK) {
        ncl_json_free(value); /* 失败时所有权仍在我们手里 */
    }
    return rc;
}

/** setValue(path, value, index, timeout)。 */
NCLSHIM_API int nclshim_client_set_value_index(const void *client, const char *path,
                                               const char *value_json, int index,
                                               unsigned timeout_ms)
{
    ncl_json *value;
    int rc;

    if (client == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    value = ncl_json_parse_cstr(value_json != NULL ? value_json : "null", NULL);
    if (value == NULL) {
        return NCL_ERR_PARSE;
    }
    rc = (int)ncl_client_set_value_index((ncl_client *)client, path, value, index,
                                         timeout_ms);
    if (rc != NCL_OK) {
        ncl_json_free(value);
    }
    return rc;
}

/**
 * 方法调用。method 形如 "/plc/setValue"（也接受 "plc/setValue"），params_json 可为
 * NULL；check=1 表示只校验参数、不执行。成功后 *out_json 收到**应答报文**的 JSON
 * 文本（code / params / data / reason），调用方用 nclshim_free 释放。
 */
NCLSHIM_API int nclshim_client_method_call(const void *client, const char *method,
                                           const char *params_json, int check,
                                           unsigned timeout_ms, char **out_json)
{
    ncl_message *request;
    ncl_message *response = NULL;
    ncl_err rc;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (client == NULL || method == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    if (request == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_message_set_method(request, method);
    if (params_json != NULL && params_json[0] != '\0') {
        ncl_json *params = ncl_json_parse_cstr(params_json, NULL);

        if (params == NULL) {
            ncl_message_free(request);
            return NCL_ERR_PARSE;
        }
        ncl_message_set_params(request, params); /* 转移所有权 */
    }
    if (check) {
        ncl_message_set_check(request, true);
    }
    rc = ncl_client_method_call((ncl_client *)client, request, timeout_ms, &response);
    if (rc != NCL_OK || response == NULL) {
        ncl_message_free(response);
        return rc != NCL_OK ? (int)rc : NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    return *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/** ping。 */
NCLSHIM_API int nclshim_client_ping(const void *client, unsigned timeout_ms)
{
    ncl_message *response = NULL;
    ncl_err rc;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_client_ping((ncl_client *)client, timeout_ms, &response);
    ncl_message_free(response);
    return (int)rc;
}

/** 路径 ↔ 节点 id 互查（都是 malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_client_get_id(const void *client, const char *path)
{
    return client != NULL && path != NULL
               ? ncl_client_get_id((ncl_client *)client, path)
               : NULL;
}

NCLSHIM_API char *nclshim_client_get_path(const void *client, const char *id)
{
    return client != NULL && id != NULL ? ncl_client_get_path((ncl_client *)client, id)
                                        : NULL;
}

/*
 * 采样/事件回调：host 是调用方给的两格数组 [函数指针, 用户数据]，托管侧用
 * GCHandle 钉住，必须活到取消订阅之后。
 */
static void nclshim_sample_thunk(ncl_client *client, const char *topic,
                                 const ncl_message *sample, void *user)
{
    nclshim_message_cb cb = NULL;
    void *cb_user = NULL;
    (void)client;

    if (user != NULL) {
        void **slots = (void **)user;
        cb = (nclshim_message_cb)slots[0];
        cb_user = slots[1];
    }
    if (cb != NULL) {
        cb(cb_user, topic, sample);
    }
}

static void nclshim_event_thunk(ncl_client *client, const char *topic,
                                const ncl_message *event, void *user)
{
    nclshim_message_cb cb = NULL;
    void *cb_user = NULL;
    (void)client;

    if (user != NULL) {
        void **slots = (void **)user;
        cb = (nclshim_message_cb)slots[0];
        cb_user = slots[1];
    }
    if (cb != NULL) {
        cb(cb_user, topic, event);
    }
}

/** 订阅采样；msg 只在回调期间有效。 */
NCLSHIM_API int nclshim_client_subscribe_samples(const void *client, int qos,
                                                 void *host)
{
    ncl_err rc;

    if (client == NULL || host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_client_subscribe_samples((ncl_client *)client, qos);
    if (rc != NCL_OK) {
        return (int)rc;
    }
    ncl_client_set_sample_handler((ncl_client *)client, nclshim_sample_thunk, host);
    return NCL_OK;
}

NCLSHIM_API int nclshim_client_unsubscribe_samples(const void *client)
{
    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_client_set_sample_handler((ncl_client *)client, NULL, NULL);
    return (int)ncl_client_unsubscribe_samples((ncl_client *)client);
}

NCLSHIM_API int nclshim_client_subscribe_events(const void *client, int qos,
                                                void *host)
{
    ncl_err rc;

    if (client == NULL || host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_client_subscribe_events((ncl_client *)client, qos);
    if (rc != NCL_OK) {
        return (int)rc;
    }
    ncl_client_set_event_handler((ncl_client *)client, nclshim_event_thunk, host);
    return NCL_OK;
}

NCLSHIM_API int nclshim_client_unsubscribe_events(const void *client)
{
    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_client_set_event_handler((ncl_client *)client, NULL, NULL);
    return (int)ncl_client_unsubscribe_events((ncl_client *)client);
}

NCLSHIM_API int nclshim_client_sample_count(const void *client)
{
    return client != NULL ? (int)ncl_client_sample_count((ncl_client *)client) : 0;
}

NCLSHIM_API int nclshim_client_event_count(const void *client)
{
    return client != NULL ? (int)ncl_client_event_count((ncl_client *)client) : 0;
}

/** 运行时加/删采样通道：config_json 是一个 SAMPLE_CHANNEL 配置节点。 */
NCLSHIM_API int nclshim_client_add_sample(const void *client, const char *config_json,
                                          unsigned timeout_ms)
{
    ncl_json *document;
    ncl_node *config;
    int rc;

    if (client == NULL || config_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    document = ncl_json_parse_cstr(config_json, NULL);
    if (document == NULL) {
        return NCL_ERR_PARSE;
    }
    config = ncl_node_from_json(document, NCL_NODE_CONFIG);
    ncl_json_free(document);
    if (config == NULL) {
        return NCL_ERR_INVALID_MODEL;
    }
    rc = (int)ncl_client_add_sample((ncl_client *)client, config, timeout_ms);
    ncl_node_free(config);
    return rc;
}

NCLSHIM_API int nclshim_client_remove_sample(const void *client, const char *id,
                                             unsigned timeout_ms)
{
    return client != NULL && id != NULL
               ? (int)ncl_client_remove_sample((ncl_client *)client, id, timeout_ms)
               : NCL_ERR_INVALID_ARG;
}
