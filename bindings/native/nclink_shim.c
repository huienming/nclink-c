/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 语言绑定共用的原生垫片（flat C ABI）：C#、Java、Python 都通过它调库。
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
 *
 * 声明见同目录的 nclink_shim.h（两份文件保持一致）。
 */

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_rest.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_topic.h"

#include "nclink_shim.h"

/*
 * 设备模型（与 C 示例共用同一份源码）：编译进垫片，于是 C# / Java / Python 的
 * 设备端示例也**不需要任何外部模型文件**。
 */
#include "../../examples/device_model.c"

/** 五个语言设备端示例共用的设备模型（JSON 文本；静态存储，不用释放）。 */
NCLSHIM_API const char *nclshim_device_model(void)
{
    return ncl_demo_device_model();
}

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

/**
 * 解析一条报文：topic 决定形态（Query/Request/...、Sample/<sn>/<通道>、
 * Event/<sn> ...），payload 是原始 MQTT 报文（不必 NUL 结尾，用长度）。失败返回
 * NULL。返回的报文**归调用方所有**，用 nclshim_message_free() 释放。
 *
 * 用途：托管侧自己拿到的 MQTT 报文（离线回放、日志、文件里存下来的样本）也能按
 * 库的规则解码，不必再手写一遍 JSON。
 */
NCLSHIM_API const void *nclshim_message_parse(const char *topic,
                                              const void *payload,
                                              int payload_len)
{
    return topic != NULL && payload != NULL && payload_len >= 0
               ? ncl_message_parse(topic, (const char *)payload,
                                   (size_t)payload_len)
               : NULL;
}

/** 释放 nclshim_message_parse() 返回的报文（借用的句柄不要传进来）。 */
NCLSHIM_API void nclshim_message_free(const void *msg)
{
    ncl_message_free((ncl_message *)msg);
}

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

/**
 * 同 nclshim_open()，但可以带上 TLS 选项（只在"库编了 TLS"时有意义）：
 * ca_file / client_cert / client_key / server_name 可为 NULL（用默认），
 * verify_peer 传 -1 表示不覆盖（默认校验）。
 */
NCLSHIM_API int nclshim_open_ex(const char *uri, const char *user, const char *pass,
                                const char *ca_file, const char *client_cert,
                                const char *client_key, const char *server_name,
                                int verify_peer)
{
    ncl_client_holder_options options;

    ncl_client_holder_options_default(&options);
    options.server_uri = uri;
    options.username = user;
    options.password = pass;
    options.tls_ca_file = ca_file;
    options.tls_client_cert = client_cert;
    options.tls_client_key = client_key;
    options.tls_server_name = server_name;
    if (verify_peer < 0) {
        options.tls_verify_peer_set = false;    /* 不覆盖，库用默认（校验） */
    } else {
        options.tls_verify_peer = verify_peer != 0;
        options.tls_verify_peer_set = true;
    }
    return (int)ncl_client_holder_init_ex(&options);
}

/** 这个垫片链的库有没有编 TLS（1 = ssl:// 可用）。 */
NCLSHIM_API int nclshim_tls_available(void)
{
    return ncl_socket_tls_available() ? 1 : 0;
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

/**
 * 把设备模型交给客户端（路径 ↔ id 互查、采样报文按模型补齐缺的 paths 都靠它）。
 *
 * 所有权：这里**先克隆一份**再交给客户端（客户端自己释放），所以调用方手里那份
 * 模型还是自己的，照常 nclshim_model_free —— 不用为"谁 free"扯皮。root 传 NULL
 * 就是清掉客户端当前装载的模型。
 */
NCLSHIM_API int nclshim_client_set_root_node(const void *client, const void *root)
{
    ncl_node *copy;

    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (root == NULL) {
        ncl_client_set_root_node((ncl_client *)client, NULL);
        return NCL_OK;
    }
    copy = ncl_node_clone((const ncl_node *)root, false);
    if (copy == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_client_set_root_node((ncl_client *)client, copy);
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
    /* ncl_client_set_value() **无条件**接管 value（成功时装进请求，失败时自己释放），
     * 所以这里绝不能再 free —— 设备拒绝写入（应答 NG）时那样会二次释放，
     * 表现为堆损坏。见 include/nclink/ncl_client.h 的"Takes ownership"。 */
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
    /* 所有权同 ncl_client_set_value()：库无条件接管，这里不要再 free。 */
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

/*
 * 异步方法调用（Method/Status、Method/Result 两对）：
 *   async 调用立刻拿应答（code=OK + handler），方法在设备端线程池里跑；
 *   拿 handler 去问 method_status / method_result。
 */

NCLSHIM_API int nclshim_client_method_call_async(const void *client,
                                                 const char *method,
                                                 const char *params_json,
                                                 unsigned timeout_ms,
                                                 char **out_json)
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
    rc = ncl_client_method_call_async((ncl_client *)client, request, timeout_ms,
                                      &response);
    if (rc != NCL_OK || response == NULL) {
        ncl_message_free(response);
        return rc != NCL_OK ? (int)rc : NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    return *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/** object_id 是报文里的 "id"（设备 id），handler 是异步调用拿到的句柄。 */
static int nclshim_client_method_query(const void *client, int want_result,
                                       const char *object_id,
                                       const char *handler, unsigned timeout_ms,
                                       char **out_json)
{
    ncl_message *response = NULL;
    ncl_err rc;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (client == NULL || handler == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = want_result
             ? ncl_client_method_result((ncl_client *)client, object_id, handler,
                                        timeout_ms, &response)
             : ncl_client_method_status((ncl_client *)client, object_id, handler,
                                        timeout_ms, &response);
    if (rc != NCL_OK || response == NULL) {
        ncl_message_free(response);
        return rc != NCL_OK ? (int)rc : NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    return *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/** 异步调用进度（process 0..100，status 取 executing/waiting/stopped/sleep）。 */
NCLSHIM_API int nclshim_client_method_status(const void *client,
                                             const char *object_id,
                                             const char *handler,
                                             unsigned timeout_ms,
                                             char **out_json)
{
    return nclshim_client_method_query(client, 0, object_id, handler, timeout_ms,
                                       out_json);
}

/** 异步调用结果：未完成 code=PENDING，完成后带 return/result 并释放句柄。 */
NCLSHIM_API int nclshim_client_method_result(const void *client,
                                             const char *object_id,
                                             const char *handler,
                                             unsigned timeout_ms,
                                             char **out_json)
{
    return nclshim_client_method_query(client, 1, object_id, handler, timeout_ms,
                                       out_json);
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

/* =============================================================== server == */

/*
 * 设备端（ncl_server）的桥。
 *
 * 托管侧只碰三样东西：不透明句柄、JSON 文本、回调。方法表与路径绑定都用 JSON
 * 描述（C 侧建表），所以托管侧仍然不需要理解任何 C 结构体布局。
 *
 * 句柄返回的是下面的 nclshim_server_ctx*（不是 ncl_server*）：服务器、我们自己
 * 建的 MQTT 连接、注册过的工具上下文都挂在它上面，nclshim_server_free() 一次收干。
 */

/** 工具方法回调：返回 ncl_err（0 = 成功）。out_result_json 为 NULL 表示"没有值"。 */
typedef int (*nclshim_tool_cb)(void *user, const char *tool, const char *method,
                               const void *params, char **out_result_json,
                               char **out_reason);

/** 自研传输的发布回调：把出站报文（主题 + 已序列化的报文体）交给托管侧。 */
typedef int (*nclshim_publish_cb)(void *user, const char *topic,
                                  const void *payload, int payload_len);

/* 一个方法一个 thunk：ncl_tool_fn 只拿到 instance，靠 thunk 的序号分辨方法。 */
#define NCLSHIM_TOOL_THUNKS 32

typedef struct nclshim_tool_ctx {
    struct nclshim_tool_ctx *next;
    char        *tool_name;
    nclshim_tool_cb cb;
    void        *user;
    size_t       method_count;
    char       **names;
} nclshim_tool_ctx;

typedef struct {
    ncl_server       *server;
    ncl_mqtt_client  *mqtt;          /* 自己建的连接（broker_url 为空时是 NULL） */
    nclshim_tool_ctx *tools;
    void             *publish_host;  /* 托管侧的自研发布回调（两格），可为 NULL */
} nclshim_server_ctx;

static ncl_err nclshim_tool_invoke(int index, void *instance,
                                   const ncl_json *params, ncl_json **result,
                                   char **reason);

#define NCLSHIM_DEFINE_TOOL_THUNK(i)                                          \
    static ncl_err nclshim_tool_thunk_##i(void *instance, const ncl_json *params, \
                                          ncl_json **result, char **reason)   \
    {                                                                         \
        return nclshim_tool_invoke(i, instance, params, result, reason);       \
    }

NCLSHIM_DEFINE_TOOL_THUNK(0)
NCLSHIM_DEFINE_TOOL_THUNK(1)
NCLSHIM_DEFINE_TOOL_THUNK(2)
NCLSHIM_DEFINE_TOOL_THUNK(3)
NCLSHIM_DEFINE_TOOL_THUNK(4)
NCLSHIM_DEFINE_TOOL_THUNK(5)
NCLSHIM_DEFINE_TOOL_THUNK(6)
NCLSHIM_DEFINE_TOOL_THUNK(7)
NCLSHIM_DEFINE_TOOL_THUNK(8)
NCLSHIM_DEFINE_TOOL_THUNK(9)
NCLSHIM_DEFINE_TOOL_THUNK(10)
NCLSHIM_DEFINE_TOOL_THUNK(11)
NCLSHIM_DEFINE_TOOL_THUNK(12)
NCLSHIM_DEFINE_TOOL_THUNK(13)
NCLSHIM_DEFINE_TOOL_THUNK(14)
NCLSHIM_DEFINE_TOOL_THUNK(15)
NCLSHIM_DEFINE_TOOL_THUNK(16)
NCLSHIM_DEFINE_TOOL_THUNK(17)
NCLSHIM_DEFINE_TOOL_THUNK(18)
NCLSHIM_DEFINE_TOOL_THUNK(19)
NCLSHIM_DEFINE_TOOL_THUNK(20)
NCLSHIM_DEFINE_TOOL_THUNK(21)
NCLSHIM_DEFINE_TOOL_THUNK(22)
NCLSHIM_DEFINE_TOOL_THUNK(23)
NCLSHIM_DEFINE_TOOL_THUNK(24)
NCLSHIM_DEFINE_TOOL_THUNK(25)
NCLSHIM_DEFINE_TOOL_THUNK(26)
NCLSHIM_DEFINE_TOOL_THUNK(27)
NCLSHIM_DEFINE_TOOL_THUNK(28)
NCLSHIM_DEFINE_TOOL_THUNK(29)
NCLSHIM_DEFINE_TOOL_THUNK(30)
NCLSHIM_DEFINE_TOOL_THUNK(31)
static ncl_tool_fn const nclshim_tool_thunks[NCLSHIM_TOOL_THUNKS] = {
    nclshim_tool_thunk_0,  nclshim_tool_thunk_1,  nclshim_tool_thunk_2,
    nclshim_tool_thunk_3,  nclshim_tool_thunk_4,  nclshim_tool_thunk_5,
    nclshim_tool_thunk_6,  nclshim_tool_thunk_7,  nclshim_tool_thunk_8,
    nclshim_tool_thunk_9,  nclshim_tool_thunk_10, nclshim_tool_thunk_11,
    nclshim_tool_thunk_12, nclshim_tool_thunk_13, nclshim_tool_thunk_14,
    nclshim_tool_thunk_15, nclshim_tool_thunk_16, nclshim_tool_thunk_17,
    nclshim_tool_thunk_18, nclshim_tool_thunk_19, nclshim_tool_thunk_20,
    nclshim_tool_thunk_21, nclshim_tool_thunk_22, nclshim_tool_thunk_23,
    nclshim_tool_thunk_24, nclshim_tool_thunk_25, nclshim_tool_thunk_26,
    nclshim_tool_thunk_27, nclshim_tool_thunk_28, nclshim_tool_thunk_29,
    nclshim_tool_thunk_30, nclshim_tool_thunk_31};

/** 回调桥：把托管侧算出来的 JSON 文本收成库要的 ncl_json，所有权交给库。 */
static ncl_err nclshim_tool_invoke(int index, void *instance, const ncl_json *params,
                                   ncl_json **result, char **reason)
{
    nclshim_tool_ctx *tool = (nclshim_tool_ctx *)instance;
    const char *method;
    char *text = NULL;
    char *why = NULL;
    int rc;

    if (result != NULL) {
        *result = NULL;
    }
    if (reason != NULL) {
        *reason = NULL;
    }
    if (tool == NULL || tool->cb == NULL || index < 0 ||
        (size_t)index >= tool->method_count) {
        return NCL_ERR_INVALID_ARG;
    }
    method = tool->names[index];
    rc = tool->cb(tool->user, tool->tool_name, method, params, &text, &why);
    if (rc != NCL_OK) {
        if (reason != NULL) {
            *reason = why;
        } else {
            free(why);
        }
        free(text);
        return (ncl_err)rc;
    }
    if (text != NULL) {
        ncl_json *value = ncl_json_parse_cstr(text, NULL);
        free(text);
        if (value == NULL) {
            if (reason != NULL) {
                *reason = why;
            } else {
                free(why);
            }
            return NCL_ERR_PARSE;
        }
        if (result != NULL) {
            *result = value;
        } else {
            ncl_json_free(value);
        }
    }
    free(why);                       /* 成功路径不带 reason */
    return NCL_OK;
}

/** 自研传输：出站报文交给托管侧（host 是托管侧给的两格 [函数指针, 用户数据]）。 */
static ncl_err nclshim_publish_thunk(void *user, const char *topic,
                                     const char *payload, size_t len)
{
    nclshim_server_ctx *ctx = (nclshim_server_ctx *)user;
    nclshim_publish_cb cb;
    void *cb_user;

    if (ctx == NULL || ctx->publish_host == NULL) {
        return NCL_ERR;
    }
    cb = (nclshim_publish_cb)((void **)ctx->publish_host)[0];
    cb_user = ((void **)ctx->publish_host)[1];
    if (cb == NULL) {
        return NCL_ERR;
    }
    return (ncl_err)cb(cb_user, topic, payload, (int)len);
}

/** MQTT 入站：解析成报文后交给 ncl_server_on_message（它接管所有权、异步应答）。 */
static void nclshim_server_on_mqtt(void *user, const ncl_mqtt_publish *publish)
{
    nclshim_server_ctx *ctx = (nclshim_server_ctx *)user;
    ncl_message *request;

    if (ctx == NULL || publish == NULL || publish->topic == NULL) {
        return;
    }
    request = ncl_message_parse(publish->topic, (const char *)publish->payload,
                                publish->payload_len);
    if (request == NULL) {
        ncl_log_warn("无法解析来自 %s 的报文", publish->topic);
        return;
    }
    ncl_server_on_message(ctx->server, publish->topic, request);
}

/**
 * 建一个设备端：模型默认走内置模型；broker_url 为空则**不接 MQTT**（离线使用
 * nclshim_server_dispatch() 驱动，或者自己给 publish_host 当传输）；
 * publish_host 非空的另一种用法是"自研传输"：每条出站报文都交给托管侧。
 *
 * 用户名/密码可为 NULL（匿名）。失败返回 NULL。
 */
/**
 * nclshim_server_create() 的带 TLS 版本：多出来的四个 TLS 参数只在 broker_url 是
 * ssl:// / tls:// 时用（ca_file 等可为 NULL；verify_peer 传 -1 = 不覆盖，库默认校验）。
 */
NCLSHIM_API const void *nclshim_server_create_ex(const char *sn, const char *model_json,
                                                 const char *broker_url,
                                                 const char *username,
                                                 const char *password,
                                                 const char *ca_file,
                                                 const char *client_cert,
                                                 const char *client_key,
                                                 const char *server_name,
                                                 int verify_peer,
                                                 void *publish_host)
{
    nclshim_server_ctx *ctx;
    ncl_server_options options;
    char *default_model = NULL;

    if (sn == NULL || sn[0] == '\0') {
        return NULL;
    }
    ctx = (nclshim_server_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->publish_host = publish_host;

    if (broker_url != NULL && broker_url[0] != '\0') {
        ncl_mqtt_client_options mqtt_options;

        ncl_mqtt_client_options_default(&mqtt_options);
        mqtt_options.url = broker_url;
        mqtt_options.client_id = sn;            /* 设备端用 SN 做 clientId */
        mqtt_options.username = username != NULL && username[0] != '\0' ? username : NULL;
        mqtt_options.password = password != NULL && password[0] != '\0' ? password : NULL;
        mqtt_options.keep_alive_seconds = 60;
        mqtt_options.automatic_reconnect = true;
        mqtt_options.tls_ca_file = ca_file;
        mqtt_options.tls_client_cert = client_cert;
        mqtt_options.tls_client_key = client_key;
        mqtt_options.tls_server_name = server_name;
        if (verify_peer >= 0) {
            mqtt_options.tls_verify_peer = verify_peer != 0;
        }
        mqtt_options.on_message = nclshim_server_on_mqtt;
        mqtt_options.user = ctx;
        ctx->mqtt = ncl_mqtt_client_create(&mqtt_options);
        if (ctx->mqtt == NULL || ncl_mqtt_client_connect(ctx->mqtt) != NCL_OK) {
            if (ctx->mqtt != NULL) {
                ncl_mqtt_client_destroy(ctx->mqtt);
            }
            free(ctx);
            return NULL;
        }
    }

    memset(&options, 0, sizeof(options));
    options.sn = sn;
    options.mqtt = ctx->mqtt;                   /* 借用：连接由我们自己释放 */
    if (model_json != NULL && model_json[0] != '\0') {
        options.model_json = model_json;
    } else {
        /*
         * 没给模型就用库内置的那份：给 ncl_server_create() 传 NULL 是"空模型"，
         * 设备端没有模型就没法采样、也没法按路径应答。
         */
        ncl_node *fallback = ncl_root_node_parse(NULL);

        if (fallback != NULL) {
            default_model = ncl_node_write_string(fallback);
            ncl_node_free(fallback);
            options.model_json = default_model;
        }
    }
    if (publish_host != NULL) {
        options.publish = nclshim_publish_thunk;
        options.publish_user = ctx;
    }
    ctx->server = ncl_server_create(&options);
    free(default_model);
    if (ctx->server == NULL) {
        if (ctx->mqtt != NULL) {
            ncl_mqtt_client_disconnect(ctx->mqtt);
            ncl_mqtt_client_destroy(ctx->mqtt);
        }
        free(ctx);
        return NULL;
    }
    return ctx;
}

/** 老入口：不带 TLS（等价于 nclshim_server_create_ex(..., NULL, NULL, NULL, NULL, -1, ...)）。 */
NCLSHIM_API const void *nclshim_server_create(const char *sn, const char *model_json,
                                              const char *broker_url,
                                              const char *username,
                                              const char *password,
                                              void *publish_host)
{
    return nclshim_server_create_ex(sn, model_json, broker_url, username, password,
                                    NULL, NULL, NULL, NULL, -1, publish_host);
}

/** 释放设备端：先停服务，再拆连接，最后放工具上下文。 */
NCLSHIM_API void nclshim_server_free(const void *handle)
{
    nclshim_server_ctx *ctx = (nclshim_server_ctx *)handle;
    nclshim_tool_ctx *tool;

    if (ctx == NULL) {
        return;
    }
    if (ctx->server != NULL) {
        ncl_server_free(ctx->server);           /* 内部停采样、FTP 与文件工具 */
    }
    if (ctx->mqtt != NULL) {
        ncl_mqtt_client_disconnect(ctx->mqtt);
        ncl_mqtt_client_destroy(ctx->mqtt);
    }
    for (tool = ctx->tools; tool != NULL; ) {
        nclshim_tool_ctx *next = tool->next;
        size_t i;

        for (i = 0; i < tool->method_count; i++) {
            free(tool->names[i]);
        }
        free(tool->names);
        free(tool->tool_name);
        free(tool);
        tool = next;
    }
    free(ctx);
}

NCLSHIM_API const char *nclshim_server_sn(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? ncl_server_sn(ctx->server) : NULL;
}

/** 设备模型（借用句柄，nclshim_node_* 都能用；服务器释放即失效）。 */
NCLSHIM_API const void *nclshim_server_model(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? ncl_server_model(ctx->server) : NULL;
}

NCLSHIM_API char *nclshim_server_model_json(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL && ctx->server != NULL
               ? ncl_node_write_string(ncl_server_model(ctx->server))
               : NULL;
}

NCLSHIM_API int nclshim_server_binding_count(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_binding_count(ctx->server) : 0;
}

NCLSHIM_API int nclshim_server_operation_count(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_operation_count(ctx->server) : 0;
}

NCLSHIM_API int nclshim_server_sample_count(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_sample_count(ctx->server) : 0;
}

NCLSHIM_API int nclshim_server_sample_upload_count(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_sample_upload_count(ctx->server) : 0;
}

NCLSHIM_API int nclshim_server_event_count(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_event_count(ctx->server) : 0;
}

NCLSHIM_API char *nclshim_server_openapi_json(const void *handle, const char *base_url)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL
               ? ncl_server_openapi_schema_json(ctx->server,
                                                base_url != NULL ? base_url : "")
               : NULL;
}

NCLSHIM_API int nclshim_server_subscribe(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_subscribe(ctx->server) : NCL_ERR_INVALID_ARG;
}

/**
 * 注册一个工具：方法表与路径绑定都用 JSON 描述——
 *
 *   methods_json:  [{"name":"getValue"},{"name":"setValue","schema":{...}}]
 *   bindings_json: [{"path":"/STATUS","operation":0,"method":"getValue"}]（可为 NULL）
 *
 * host 是托管侧给的两格 [工具回调, 用户数据]，和采样/事件回调一个约定；回调的
 * 生命周期必须覆盖到 nclshim_server_free()。一个工具最多 32 个方法。
 */
NCLSHIM_API int nclshim_server_register_tool(const void *handle, const char *tool_name,
                                             const char *methods_json,
                                             const char *bindings_json, void *host)
{
    nclshim_server_ctx *ctx = (nclshim_server_ctx *)handle;
    ncl_json *doc = NULL;
    ncl_json *binds = NULL;
    ncl_tool_method *methods = NULL;
    ncl_tool_binding *specs = NULL;
    char **schemas = NULL;
    nclshim_tool_ctx *tool = NULL;
    void **slots = (void **)host;
    size_t count = 0;
    size_t binding_count = 0;
    size_t i;
    ncl_err rc = NCL_OK;

    if (ctx == NULL || ctx->server == NULL || tool_name == NULL ||
        methods_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    doc = ncl_json_parse_cstr(methods_json, NULL);
    if (doc == NULL || ncl_json_type_of(doc) != NCL_JSON_ARRAY) {
        ncl_json_free(doc);
        return NCL_ERR_PARSE;
    }
    count = ncl_json_arr_len(doc);
    if (count == 0 || count > NCLSHIM_TOOL_THUNKS) {
        ncl_json_free(doc);
        return NCL_ERR_INVALID_ARG;             /* 一个工具最多 32 个方法 */
    }

    methods = (ncl_tool_method *)calloc(count, sizeof(*methods));
    schemas = (char **)calloc(count, sizeof(*schemas));
    tool = (nclshim_tool_ctx *)calloc(1, sizeof(*tool));
    if (methods == NULL || schemas == NULL || tool == NULL) {
        rc = NCL_ERR_NOMEM;
        goto done;
    }
    tool->names = (char **)calloc(count, sizeof(char *));
    if (tool->names == NULL) {
        rc = NCL_ERR_NOMEM;
        goto done;
    }
    tool->cb = slots != NULL ? (nclshim_tool_cb)slots[0] : NULL;
    tool->user = slots != NULL ? slots[1] : NULL;
    tool->method_count = count;
    tool->tool_name = ncl_strdup(tool_name);
    if (tool->tool_name == NULL) {
        rc = NCL_ERR_NOMEM;
        goto done;
    }
    for (i = 0; i < count; i++) {
        const ncl_json *entry = ncl_json_arr_get(doc, i);
        const char *name = ncl_json_obj_get_string(entry, "name");
        const ncl_json *schema = ncl_json_obj_get(entry, "schema");

        if (name == NULL) {
            rc = NCL_ERR_INVALID_ARG;
            goto done;
        }
        tool->names[i] = ncl_strdup(name);
        if (tool->names[i] == NULL) {
            rc = NCL_ERR_NOMEM;
            goto done;
        }
        methods[i].name = tool->names[i];
        methods[i].fn = nclshim_tool_thunks[i];
        if (schema != NULL && !ncl_json_is_null(schema)) {
            schemas[i] = ncl_json_write_string(schema);
            methods[i].params_schema = schemas[i];
        }
    }

    if (bindings_json != NULL && bindings_json[0] != '\0') {
        binds = ncl_json_parse_cstr(bindings_json, NULL);
        if (binds == NULL || ncl_json_type_of(binds) != NCL_JSON_ARRAY) {
            rc = NCL_ERR_PARSE;
            goto done;
        }
        binding_count = ncl_json_arr_len(binds);
        if (binding_count > 0) {
            specs = (ncl_tool_binding *)calloc(binding_count, sizeof(*specs));
            if (specs == NULL) {
                rc = NCL_ERR_NOMEM;
                goto done;
            }
            for (i = 0; i < binding_count; i++) {
                const ncl_json *entry = ncl_json_arr_get(binds, i);
                const char *path = ncl_json_obj_get_string(entry, "path");
                const char *method = ncl_json_obj_get_string(entry, "method");
                long long op = 0;

                if (path == NULL || method == NULL) {
                    continue;
                }
                (void)ncl_json_as_int(ncl_json_obj_get(entry, "operation"), &op);
                specs[i].path = path;
                specs[i].operation = (ncl_operation)op;
                specs[i].method = method;
                specs[i].tool = tool_name;
            }
        }
    }

    rc = ncl_server_register_tool(ctx->server, tool_name, tool, methods, count, specs,
                                  binding_count);
    if (rc == NCL_OK) {
        tool->next = ctx->tools;                /* instance 要活到 server 释放 */
        ctx->tools = tool;
        tool = NULL;
    }

done:
    if (tool != NULL) {
        size_t n = tool->method_count;

        for (i = 0; i < n; i++) {
            free(tool->names[i]);
        }
        free(tool->names);
        free(tool->tool_name);
        free(tool);
    }
    for (i = 0; i < count; i++) {
        free(schemas[i]);
    }
    free(schemas);
    free(methods);
    free(specs);
    ncl_json_free(binds);
    ncl_json_free(doc);
    return (int)rc;
}

NCLSHIM_API int nclshim_server_register_builtin_tool(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_register_builtin_tool(ctx->server)
                       : NCL_ERR_INVALID_ARG;
}

NCLSHIM_API int nclshim_server_register_file_tool(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_register_file_tool(ctx->server)
                       : NCL_ERR_INVALID_ARG;
}

NCLSHIM_API int nclshim_server_start_ftp(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_start_ftp(ctx->server) : NCL_ERR_INVALID_ARG;
}

/**
 * 离线驱动一条请求：payload 是原始报文体（不必 NUL 结尾），返回应答报文的 JSON
 * 文本（malloc，调用方释放）。**不经过 MQTT**，也不发布应答 —— 单测与"自己当
 * 传输"的宿主用它。
 */
NCLSHIM_API int nclshim_server_dispatch(const void *handle, const char *topic,
                                        const void *payload, int payload_len,
                                        char **out_json)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_message *request;
    ncl_message *response;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (ctx == NULL || topic == NULL || payload == NULL || payload_len < 0 ||
        out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    request = ncl_message_parse(topic, (const char *)payload, (size_t)payload_len);
    if (request == NULL) {
        return NCL_ERR_PARSE;
    }
    response = ncl_server_dispatch(ctx->server, topic, request);
    ncl_message_free(request);
    if (response == NULL) {
        return NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    return *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/** 离线调用一个工具方法（不经过 MQTT）；check=1 只校验参数。 */
static int nclshim_server_call_method(const void *handle, const char *method,
                                      const char *params_json, int check,
                                      char **out_json)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_message *request;
    ncl_message *response;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (ctx == NULL || method == NULL || out_json == NULL) {
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
        ncl_message_set_params(request, params);        /* 转移所有权 */
    }
    if (check) {
        ncl_message_set_check(request, true);
    }
    response = check ? ncl_server_check_method_call(ctx->server, request)
                     : ncl_server_invoke_method_call(ctx->server, request);
    ncl_message_free(request);
    if (response == NULL) {
        return NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    return *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

NCLSHIM_API int nclshim_server_invoke_method_call(const void *handle,
                                                  const char *method,
                                                  const char *params_json,
                                                  char **out_json)
{
    return nclshim_server_call_method(handle, method, params_json, 0, out_json);
}

NCLSHIM_API int nclshim_server_check_method_call(const void *handle,
                                                 const char *method,
                                                 const char *params_json,
                                                 char **out_json)
{
    return nclshim_server_call_method(handle, method, params_json, 1, out_json);
}

/*
 * 离线驱动异步方法调用：设备端自己走一遍 async 调用与两条查询（不需要 broker），
 * 便于托管侧自检。真机上这三步分别由 Method/Call|Status|Result 请求触发。
 */

/** 发起一次异步调用；应答里 code=OK + handler（方法在池里跑）。 */
NCLSHIM_API int nclshim_server_invoke_method_call_async(const void *handle,
                                                        const char *method,
                                                        const char *params_json,
                                                        char **out_json)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_message *request;
    ncl_message *response;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (ctx == NULL || method == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    if (request == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_message_set_method(request, method);
    ncl_message_set_async(request, true);
    if (params_json != NULL && params_json[0] != '\0') {
        ncl_json *params = ncl_json_parse_cstr(params_json, NULL);

        if (params == NULL) {
            ncl_message_free(request);
            return NCL_ERR_PARSE;
        }
        ncl_message_set_params(request, params); /* 转移所有权 */
    }
    response = ncl_server_invoke_method_call(ctx->server, request);
    ncl_message_free(request);
    if (response == NULL) {
        return NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    return *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

/** 按句柄查状态/结果：内部就是设备端对那两条请求的应答。 */
static int nclshim_server_method_query(const void *handle, int want_result,
                                       const char *object_id,
                                       const char *handler, char **out_json)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_message *request;
    ncl_message *response;
    char *topic;
    int rc = NCL_OK;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (ctx == NULL || handler == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    request = ncl_message_new(want_result ? NCL_MSG_METHOD_RESULT_REQUEST
                                          : NCL_MSG_METHOD_STATUS_REQUEST);
    if (request == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_message_set_request_id(request, object_id);
    ncl_message_set_handler(request, handler);
    topic = want_result
                ? ncl_topic_method_result_request(ncl_server_sn(ctx->server), NULL)
                : ncl_topic_method_status_request(ncl_server_sn(ctx->server), NULL);
    if (topic == NULL) {
        ncl_message_free(request);
        return NCL_ERR_NOMEM;
    }
    response = ncl_server_dispatch(ctx->server, topic, request);
    ncl_mem_free(topic);
    ncl_message_free(request);
    if (response == NULL) {
        return NCL_ERR;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    if (*out_json == NULL) {
        rc = NCL_ERR_NOMEM;
    }
    return rc;
}

NCLSHIM_API int nclshim_server_invoke_method_status(const void *handle,
                                                    const char *object_id,
                                                    const char *handler,
                                                    char **out_json)
{
    return nclshim_server_method_query(handle, 0, object_id, handler, out_json);
}

NCLSHIM_API int nclshim_server_invoke_method_result(const void *handle,
                                                    const char *object_id,
                                                    const char *handler,
                                                    char **out_json)
{
    return nclshim_server_method_query(handle, 1, object_id, handler, out_json);
}

/** 异步方法调用上报进度（可选；不报就是 executing/stopped + process=0）。 */
NCLSHIM_API int nclshim_server_report_method_progress(const void *handle,
                                                      const char *handler,
                                                      long long process,
                                                      const char *status)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL
               ? (int)ncl_server_report_method_progress(ctx->server, handler,
                                                        process, status)
               : NCL_ERR_INVALID_ARG;
}

NCLSHIM_API int nclshim_server_init_samples(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL ? (int)ncl_server_init_samples(ctx->server)
                       : NCL_ERR_INVALID_ARG;
}

/** 运行时加一个采样通道（config_json 是一个 SAMPLE_CHANNEL 配置节点）。 */
NCLSHIM_API int nclshim_server_add_sample(const void *handle, const char *config_json)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_json *document;
    ncl_node *config;
    int rc;

    if (ctx == NULL || config_json == NULL) {
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
    rc = (int)ncl_server_add_sample(ctx->server, config);
    ncl_node_free(config);
    return rc;
}

NCLSHIM_API int nclshim_server_remove_sample(const void *handle, const char *id)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL && id != NULL
               ? (int)ncl_server_remove_sample(ctx->server, id)
               : NCL_ERR_INVALID_ARG;
}

NCLSHIM_API void nclshim_server_stop_all_samples(const void *handle)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    if (ctx != NULL) {
        ncl_server_stop_all_samples(ctx->server);
    }
}

/** 推一条事件到 Event/<sn>；event_json 形如 {"key":...,"value":...}。 */
NCLSHIM_API int nclshim_server_push_event(const void *handle, const char *event_id,
                                          const char *event_json)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_json *event;
    int rc;

    if (ctx == NULL || event_id == NULL || event_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    event = ncl_json_parse_cstr(event_json, NULL);
    if (event == NULL) {
        return NCL_ERR_PARSE;
    }
    rc = (int)ncl_server_push_event(ctx->server, event_id, event);
    ncl_json_free(event);
    return rc;
}

NCLSHIM_API int nclshim_server_push_event_ex(const void *handle, const char *event_id,
                                             const char *event_json,
                                             long long time_ms,
                                             const char *message_id)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;
    ncl_json *event;
    int rc;

    if (ctx == NULL || event_id == NULL || event_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    event = ncl_json_parse_cstr(event_json, NULL);
    if (event == NULL) {
        return NCL_ERR_PARSE;
    }
    rc = (int)ncl_server_push_event_ex(ctx->server, event_id, event, time_ms,
                                       message_id);
    ncl_json_free(event);
    return rc;
}
/**
 * 复制一份文本（malloc，调用方 nclshim_free）。**回调里回填字符串必须用它**：
 * 垫片随后会用自己这边的 free() 释放，托管侧自己 malloc/python bytes/GC 出来的
 * 内存不能这么用（跨 CRT / 跨分配器）。
 */
NCLSHIM_API char *nclshim_strdup(const char *text)
{
    return text != NULL ? ncl_strdup(text) : NULL;
}

/* ============================================================== http/rest == */

/*
 * HTTP / REST 端点。
 *
 * 设备端的两组 REST 端点都由库自己注册：
 *   ncl_rest_attach()        GET /api/schema、/swagger-ui、POST /api/<工具>/<方法>
 *   ncl_rest_attach_config() GET/POST /api/cfg/*  与 /api/method/*（SN、模型、驱动、
 *                            mqtt.cfg、服务器列表）
 * 托管侧另外可以自己挂路由（nclshim_http_route），回调拿 method/path/query/body，
 * 回 status/content_type/body —— 一律 UTF-8 文本，够写普通 REST 了。
 */

/** 自定义路由回调：返回 0 表示成功。out_* 里是 malloc 出来的文本（垫片释放）。 */
typedef int (*nclshim_route_cb)(void *user, const char *method, const char *path,
                                const char *query, const char *body, int *out_status,
                                char **out_content_type, char **out_body);

typedef struct nclshim_route_ctx {
    struct nclshim_route_ctx *next;
    void *host;                     /* 托管侧给的两格 [函数指针, 用户数据] */
} nclshim_route_ctx;

typedef struct {
    ncl_http_server *http;
    nclshim_route_ctx *routes;      /* 自己挂的路由（回调要活到 http_free） */
} nclshim_http_ctx;

static void nclshim_http_route_thunk(ncl_http_request *request,
                                     ncl_http_response *response, void *user)
{
    nclshim_route_cb cb = (nclshim_route_cb)((void **)user)[0];
    void *cb_user = ((void **)user)[1];
    int status = 200;
    char *content_type = NULL;
    char *body = NULL;

    if (cb == NULL) {
        ncl_http_reply_text(response, 500, "no handler");
        return;
    }
    if (cb(cb_user, ncl_http_method(request), ncl_http_path(request),
           ncl_http_query_string(request), ncl_http_body(request), &status,
           &content_type, &body) != 0) {
        /* 回调失败：状态码至少 5xx，但托管侧给的报文照发（它更清楚出了什么事）。 */
        if (status < 400) {
            status = 500;
        }
        if (body == NULL) {
            body = ncl_strdup("handler failed");
            content_type = ncl_strdup("text/plain; charset=utf-8");
        }
    }
    if (body == NULL) {
        ncl_http_set_status(response, status);
        free(content_type);
        return;
    }
    ncl_http_reply(response, status, content_type, body, strlen(body));
    free(content_type);
    free(body);
}

/**
 * 起一个 HTTP 端点：port=0 用随机端口（端口用 nclshim_http_port 查）。
 * server 非空时挂 REST（OpenAPI + swagger-ui + 工具端点）；with_config 非 0 再
 * 挂配置端点（SN/模型/驱动/mqtt.cfg/服务器列表）。失败返回 NULL。
 */
NCLSHIM_API const void *nclshim_http_start(unsigned port, const void *server,
                                           int with_config)
{
    nclshim_http_ctx *ctx = (nclshim_http_ctx *)calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->http = ncl_http_server_create(port);
    if (ctx->http == NULL) {
        free(ctx);
        return NULL;
    }
    if (server != NULL) {
        if (ncl_rest_attach(ctx->http, ((nclshim_server_ctx *)server)->server) != NCL_OK) {
            ncl_http_server_free(ctx->http);
            free(ctx);
            return NULL;
        }
        if (with_config && ncl_rest_attach_config(ctx->http) != NCL_OK) {
            ncl_http_server_free(ctx->http);
            free(ctx);
            return NULL;
        }
    }
    if (ncl_http_server_start(ctx->http) != NCL_OK) {
        ncl_http_server_free(ctx->http);
        free(ctx);
        return NULL;
    }
    return ctx;
}

NCLSHIM_API void nclshim_http_free(const void *handle)
{
    nclshim_http_ctx *ctx = (nclshim_http_ctx *)handle;

    if (ctx == NULL) {
        return;
    }
    ncl_http_server_stop(ctx->http);
    ncl_http_server_free(ctx->http);
    while (ctx->routes != NULL) {
        nclshim_route_ctx *next = ctx->routes->next;
        free(ctx->routes);
        ctx->routes = next;
    }
    free(ctx);
}

NCLSHIM_API int nclshim_http_port(const void *handle)
{
    const nclshim_http_ctx *ctx = (const nclshim_http_ctx *)handle;

    return ctx != NULL ? (int)ncl_http_server_port(ctx->http) : 0;
}

NCLSHIM_API int nclshim_http_request_count(const void *handle)
{
    const nclshim_http_ctx *ctx = (const nclshim_http_ctx *)handle;

    return ctx != NULL ? (int)ncl_http_server_request_count(ctx->http) : 0;
}

NCLSHIM_API void nclshim_http_set_cors(const void *handle, int enabled)
{
    const nclshim_http_ctx *ctx = (const nclshim_http_ctx *)handle;

    if (ctx != NULL) {
        ncl_http_server_set_cors(ctx->http, enabled != 0);
    }
}

/** 挂一条自己的路由：method 支持 "*"（见 ncl_http_server_route 的匹配规则）。 */
NCLSHIM_API int nclshim_http_route(const void *handle, const char *method,
                                   const char *path, void *host)
{
    nclshim_http_ctx *ctx = (nclshim_http_ctx *)handle;
    nclshim_route_ctx *route;
    ncl_err rc;

    if (ctx == NULL || method == NULL || path == NULL || host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_http_server_route(ctx->http, method, path, nclshim_http_route_thunk,
                               host);
    if (rc != NCL_OK) {
        return (int)rc;
    }
    route = (nclshim_route_ctx *)calloc(1, sizeof(*route));
    if (route == NULL) {
        return NCL_ERR_NOMEM;       /* 路由已经挂上了，只是回调的宿主记不住 */
    }
    route->host = host;
    route->next = ctx->routes;
    ctx->routes = route;
    return NCL_OK;
}

/* ============================================================== file/fs == */

/*
 * 文件通道：MQTT 报文里只传 "/temp/<名字>" 这样的令牌，字节走 FTP。
 *
 *   设备端（收/发文件）：nclshim_server_register_file_tool() +
 *                        nclshim_server_start_ftp()（读 bin/ftp.txt 起端点）
 *   客户端（传/收文件）：进程级 FTP 端点（127.0.0.1:2323，admin/123456，根 =
 *                        安装根）+ 每个客户端一份文件通道（客户端管理器建客户端
 *                        时已经装好）+ **文件通道握手**：先
 *                        nclshim_client_file_channel_open() 把端点交给设备（设备
 *                        于是往这儿拨 FTP），传完 ..._close() 收回租约并撤销临时
 *                        账号。nclshim_open() 不再顺手起端点（打开通道时按需起）。
 *
 * 路径基准：
 *   设备侧  <root>/uploadFile/<相对路径>         对端看到的目录树是 /<sn>/<相对>
 *   客户端  <root>/<sn>/<相对路径>               要上传的文件先放这儿
 * 即同一个相对路径（如 "/demo.txt"）在两边各自落到上面两个位置。
 *
 * 相对于设备：设备是 FTP 客户端、托管侧是 FTP 服务端。通道里广播的地址默认是
 * "到 broker 的本机地址"（回环时取本机 LAN 地址）+ 进程级端点端口；跨网段或端口
 * 映射的场合用 ..._channel_open_ex() 显式指定，或者写进 <root>/conf/ftp.txt
 * （host / port / advertisePort / root / userName / password / path / force，
 * 全可选；优先级：函数参数 > 文件 > 推导默认）。
 */

/** 起进程级 FTP 端点（幂等；nclshim_open 也会起）。 */
NCLSHIM_API int nclshim_file_start_ftp(void)
{
    return (int)ncl_client_holder_start_ftp();
}

/**
 * 同上，但可以换端口 / 根目录 / 账号：port=0、root/user/password 为 NULL 就是默认
 * （2323、安装根、admin / 123456）。托管侧与 broker 不在同一台机器、或者 2323 被占
 * 的时候用它。
 */
NCLSHIM_API int nclshim_file_start_ftp_ex(unsigned port, const char *root,
                                          const char *user, const char *password)
{
    return (int)ncl_client_holder_start_ftp_ex(port, root, user, password);
}

/**
 * 给设备端文件通道指定一个静态对端（不握手）。要求对端自己跑 FTP 服务端、目录布局
 * 是 "/<sn>/..."。host 必填；port=0 / user / password 为 NULL 用默认。对端用
 * nclshim_client_file_channel_open() 握手开的通道会顶替它。
 */
NCLSHIM_API int nclshim_server_set_file_peer(const void *handle, const char *host,
                                             unsigned port, const char *user,
                                             const char *password)
{
    const nclshim_server_ctx *ctx = (const nclshim_server_ctx *)handle;

    return ctx != NULL && ctx->server != NULL
               ? (int)ncl_server_set_file_peer(ctx->server, host, port, user, password)
               : NCL_ERR_INVALID_ARG;
}

NCLSHIM_API void nclshim_file_stop_ftp(void)
{
    ncl_client_holder_stop_ftp();
}

/**
 * 开文件通道（file/openFileChannel）：把本进程的 FTP 端点交给设备，设备随即往那儿
 * 拨 FTP。通道是租约，关掉（..._channel_close()）之前一直是这条对端。
 *
 * 默认：地址 = 到 broker 的本机地址（拿不到就 127.0.0.1），端口 = 进程级端点端口
 * （没起就按 2323 起），账号 = 库临时生成的、只有设备知道的一对（关闭时撤销）。
 * 幂等：已经开着就直接返回 0。
 */
NCLSHIM_API int nclshim_client_file_channel_open(const void *client)
{
    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return (int)ncl_client_open_file_channel((ncl_client *)client, NULL);
}

/**
 * 同上，但显式给出设备要拨的 host / port 和账号（host 必填，port 必填）。
 * 对端不在这台机器上、或者端口有映射时用它；user/password 指向的若是本进程端点，
 * 库会把账号加上并在关闭时撤销。
 */
NCLSHIM_API int nclshim_client_file_channel_open_ex(const void *client,
                                                    const char *host,
                                                    unsigned port,
                                                    const char *user,
                                                    const char *password)
{
    ncl_file_channel_options options;

    if (client == NULL || host == NULL || host[0] == '\0') {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_file_channel_options_default(&options);
    options.host = host;
    options.port = port;
    options.user = user;
    options.password = password;
    return (int)ncl_client_open_file_channel((ncl_client *)client, &options);
}

/** 收回文件通道：file/closeFileChannel + 撤销库给这条通道加的账号（幂等）。 */
NCLSHIM_API int nclshim_client_file_channel_close(const void *client)
{
    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return (int)ncl_client_close_file_channel((ncl_client *)client);
}

/** 这条客户端手上有没有文件通道（1/0）。 */
NCLSHIM_API int nclshim_client_file_channel_is_open(const void *client)
{
    return client != NULL &&
                   ncl_client_file_channel_is_open((ncl_client *)client)
               ? 1
               : 0;
}

/**
 * 保证文件通道开着：没开就按默认开一次（幂等）。绑定里的上传/下载/列目录等
 * 便利方法先调它，省得每个调用方都记得先 open。
 */
NCLSHIM_API int nclshim_client_ensure_file_channel(const void *client)
{
    if (client == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_client_file_channel_is_open((ncl_client *)client)) {
        return NCL_OK;
    }
    return (int)ncl_client_open_file_channel((ncl_client *)client, NULL);
}

/**
 * 上传一个文件：local_file_path 是**相对路径**（形如 "/demo.txt"），文件必须在
 * <root>/<sn><相对路径> 上（与 C API 一致）。
 */
NCLSHIM_API int nclshim_client_file_write(const void *client,
                                          const char *local_file_path)
{
    int rc;

    if (client == NULL || local_file_path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = nclshim_client_ensure_file_channel(client);
    if (rc != NCL_OK) {
        return rc;
    }
    return (int)ncl_client_write((ncl_client *)client, local_file_path);
}

/** 下载一个文件；返回落盘后的本地绝对路径（malloc，调用方 nclshim_free）。 */
NCLSHIM_API char *nclshim_client_file_read(const void *client,
                                           const char *remote_file_path)
{
    if (client == NULL || remote_file_path == NULL) {
        return NULL;
    }
    if (nclshim_client_ensure_file_channel(client) != NCL_OK) {
        return NULL;
    }
    return ncl_client_read((ncl_client *)client, remote_file_path);
}

/** 列目录：返回属性数组的 JSON 文本（malloc）；失败返回 NULL。 */
NCLSHIM_API char *nclshim_client_file_ll_json(const void *client,
                                              const char *remote_dir)
{
    ncl_ptrvec files;
    ncl_json *array;
    char *json = NULL;

    if (client == NULL || remote_dir == NULL) {
        return NULL;
    }
    if (nclshim_client_ensure_file_channel(client) != NCL_OK) {
        return NULL;
    }
    ncl_ptrvec_init(&files, ncl_file_attribute_release);
    if (ncl_client_ll((ncl_client *)client, remote_dir, &files) != NCL_OK) {
        ncl_ptrvec_free(&files);
        return NULL;
    }
    array = ncl_file_attributes_to_json(&files);
    if (array != NULL) {
        json = ncl_json_write_string(array);
        ncl_json_free(array);
    }
    ncl_ptrvec_free(&files);
    return json;
}

NCLSHIM_API int nclshim_client_file_mkdir(const void *client, const char *remote_dir)
{
    ncl_file_client_tool *tool;
    int rc;

    if (client == NULL || remote_dir == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = nclshim_client_ensure_file_channel(client);
    if (rc != NCL_OK) {
        return rc;
    }
    tool = ncl_client_file_tool((ncl_client *)client);
    if (tool == NULL) {
        return NCL_ERR_STATE;               /* 没装文件通道 */
    }
    return ncl_file_client_tool_mkdir(tool, remote_dir) ? NCL_OK : NCL_ERR;
}

NCLSHIM_API int nclshim_client_file_delete(const void *client,
                                           const char *remote_file_path)
{
    ncl_file_client_tool *tool;
    int rc;

    if (client == NULL || remote_file_path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = nclshim_client_ensure_file_channel(client);
    if (rc != NCL_OK) {
        return rc;
    }
    tool = ncl_client_file_tool((ncl_client *)client);
    if (tool == NULL) {
        return NCL_ERR_STATE;
    }
    return ncl_file_client_tool_delete(tool, remote_file_path) ? NCL_OK : NCL_ERR;
}

/**
 * 带文件参数的方法调用：keys_json / paths_json 是等长的字符串数组，params[keys[i]]
 * 会先换成 "/temp/<名字>"（文件从 paths_json[i] 经文件通道送过去），应答里
 * "fileKeys" 列出的键会被换成本地路径。应答 JSON 文本由调用方 nclshim_free。
 */
NCLSHIM_API int nclshim_client_method_call_file(const void *client,
                                                const char *method,
                                                const char *params_json,
                                                const char *keys_json,
                                                const char *paths_json,
                                                unsigned timeout_ms,
                                                char **out_json)
{
    ncl_message *request = NULL;
    ncl_message *response = NULL;
    ncl_json *keys_doc = NULL;
    ncl_json *paths_doc = NULL;
    const char **keys = NULL;
    const char **paths = NULL;
    size_t count;
    size_t i;
    ncl_err rc = NCL_OK;

    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (client == NULL || method == NULL || keys_json == NULL ||
        paths_json == NULL || out_json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (nclshim_client_ensure_file_channel(client) != NCL_OK) {
        return NCL_ERR_NO_CHANNEL;
    }
    keys_doc = ncl_json_parse_cstr(keys_json, NULL);
    paths_doc = ncl_json_parse_cstr(paths_json, NULL);
    if (keys_doc == NULL || paths_doc == NULL) {
        rc = NCL_ERR_PARSE;
        goto done;
    }
    count = ncl_json_arr_len(keys_doc);
    if (ncl_json_arr_len(paths_doc) != count) {
        rc = NCL_ERR_INVALID_ARG;
        goto done;
    }
    if (count > 0) {
        keys = (const char **)calloc(count, sizeof(*keys));
        paths = (const char **)calloc(count, sizeof(*paths));
        if (keys == NULL || paths == NULL) {
            rc = NCL_ERR_NOMEM;
            goto done;
        }
        for (i = 0; i < count; i++) {
            keys[i] = ncl_json_as_string(ncl_json_arr_get(keys_doc, i));
            paths[i] = ncl_json_as_string(ncl_json_arr_get(paths_doc, i));
            if (keys[i] == NULL || paths[i] == NULL) {
                rc = NCL_ERR_INVALID_ARG;   /* 借用：文档活着就有效 */
                goto done;
            }
        }
    }
    request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    if (request == NULL) {
        rc = NCL_ERR_NOMEM;
        goto done;
    }
    ncl_message_set_method(request, method);
    if (params_json != NULL && params_json[0] != '\0') {
        ncl_json *params = ncl_json_parse_cstr(params_json, NULL);

        if (params == NULL) {
            ncl_message_free(request);
            request = NULL;
            rc = NCL_ERR_PARSE;
            goto done;
        }
        ncl_message_set_params(request, params);    /* 转移所有权 */
    }
    /* 这个调用接管 request（成功失败都一样），别再自己释放。 */
    rc = ncl_client_method_call_file((ncl_client *)client, request, keys, paths,
                                     count, timeout_ms, &response);
    request = NULL;
    if (rc != NCL_OK || response == NULL) {
        ncl_message_free(response);
        rc = rc != NCL_OK ? rc : NCL_ERR;
        goto done;
    }
    *out_json = ncl_message_write_string(response);
    ncl_message_free(response);
    rc = *out_json != NULL ? NCL_OK : NCL_ERR_NOMEM;

done:
    free(keys);
    free(paths);
    ncl_json_free(keys_doc);
    ncl_json_free(paths_doc);
    return (int)rc;
}

/* ---------------------------------------------------------- 文件小工具 -- */

NCLSHIM_API int nclshim_file_need_compression(const char *file_name)
{
    return file_name != NULL && ncl_file_need_compression(file_name) ? 1 : 0;
}

NCLSHIM_API int nclshim_file_total_chunks(long long size)
{
    return size > 0 ? ncl_file_total_chunks(size) : 0;
}

/** 文件内容的 SHA-256（小写十六进制，malloc；调用方 nclshim_free）。 */
NCLSHIM_API char *nclshim_file_checksum(const char *path)
{
    char *hex = NULL;

    if (path == NULL) {
        return NULL;
    }
    return ncl_file_checksum(path, &hex) == NCL_OK ? hex : NULL;
}

/** 本地文件的属性（一个 JSON 对象；字段顺序按规范固定）：见 ncl_file_attribute_to_json。 */
NCLSHIM_API char *nclshim_file_attribute_json(const char *path, const char *parent)
{
    ncl_file_attribute *attribute = NULL;
    ncl_json *json;
    char *text = NULL;

    if (path == NULL) {
        return NULL;
    }
    if (ncl_file_attribute_of(path, parent, &attribute) != NCL_OK ||
        attribute == NULL) {
        return NULL;
    }
    json = ncl_file_attribute_to_json(attribute);
    if (json != NULL) {
        text = ncl_json_write_string(json);
        ncl_json_free(json);
    }
    ncl_file_attribute_free(attribute);
    return text;
}
