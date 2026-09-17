/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * nclink_shim 的声明（生成物：python tools/gen_shim_header.py）。
 *
 * 这个头文件是给**编译期**用的：Java 的 JNI 胶水（nclink_jni.c）把它和
 * nclink_shim.c 一起编进 nclink_jni.dll。C# / Python 走的是运行时查找（P/Invoke /
 * ctypes），只需要保证垫片 DLL 在搜索路径上。
 *
 * 约定见 nclink_shim.c 顶部：返回 char* 的要 nclshim_free()，返回 const char* /
 * const void* 的是借用指针，回调里的 message 句柄只在回调期间有效。
 */
#ifndef NCLINK_SHIM_H
#define NCLINK_SHIM_H

#if defined(_WIN32)
#define NCLSHIM_API __declspec(dllexport)
#else
#define NCLSHIM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 库版本号（编译期常量，不用释放）。 */
NCLSHIM_API const char *nclshim_version(void);

/** 错误码文本。 */
NCLSHIM_API const char *nclshim_err_name(int code);

/** 释放垫片返还的所有权内存（char* / JSON / 模型）。 */
NCLSHIM_API void nclshim_free(void *ptr);

/** 安装根目录（conf/、bin/、log/ 都在它下面）。 */
NCLSHIM_API void nclshim_env_set_root(const char *root);

NCLSHIM_API const char *nclshim_env_root(void);

/** 日志：NULL/空目录 = 默认的 <root>/log。返回 1 成功。 */
NCLSHIM_API int nclshim_log_init(const char *dir);

NCLSHIM_API void nclshim_log_shutdown(void);

/** 0=Debug 1=Info 2=Warn 3=Error 4=Fatal（见 ncl_log_level）。 */
NCLSHIM_API void nclshim_log_set_level(int level);

NCLSHIM_API void nclshim_log_set_console(int enabled);

/** 解析 JSON 文本；失败返回 NULL（不抛异常）。 */
NCLSHIM_API const void *nclshim_json_parse(const char *text);

NCLSHIM_API const void *nclshim_json_clone(const void *json);

NCLSHIM_API void nclshim_json_free(const void *json);

/** 紧凑 JSON 文本（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_json_write(const void *json);

/** ncl_json_type：0 null / 1 bool / 2 number / 3 string / 4 array / 5 object。 */
NCLSHIM_API int nclshim_json_type(const void *json);

NCLSHIM_API int nclshim_json_is_null(const void *json);

NCLSHIM_API int nclshim_json_as_int(const void *json, long long *out);

NCLSHIM_API int nclshim_json_as_double(const void *json, double *out);

NCLSHIM_API int nclshim_json_as_bool(const void *json, int *out);

/** 字符串值（借用；不是字符串返回 NULL）。 */
NCLSHIM_API const char *nclshim_json_string(const void *json);

/** 文本形式：数字原样、字符串去引号（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_json_text(const void *json);

/** 数组元素个数 / 对象成员个数。 */
NCLSHIM_API int nclshim_json_count(const void *json);

NCLSHIM_API const void *nclshim_json_array_get(const void *json, int index);

NCLSHIM_API const void *nclshim_json_object_value_at(const void *json, int index);

NCLSHIM_API const char *nclshim_json_object_key_at(const void *json, int index);

NCLSHIM_API const void *nclshim_json_object_get(const void *json, const char *key);

/** 解析模型文档（NULL 或空串 = 库内置默认模型）。调用方用 nclshim_model_free。 */
NCLSHIM_API const void *nclshim_model_parse(const char *json);

NCLSHIM_API void nclshim_model_free(const void *root);

/** 序列化整棵树（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_model_write(const void *root);

NCLSHIM_API const void *nclshim_model_find_by_id(const void *root, const char *id);

NCLSHIM_API int nclshim_node_type(const void *node);

NCLSHIM_API const char *nclshim_node_type_name(const void *node);

NCLSHIM_API const char *nclshim_node_name(const void *node);

NCLSHIM_API const char *nclshim_node_id(const void *node);

NCLSHIM_API const char *nclshim_node_path(const void *node);

NCLSHIM_API const char *nclshim_node_description(const void *node);

NCLSHIM_API const char *nclshim_node_number(const void *node);

NCLSHIM_API const char *nclshim_node_data_type(const void *node);

NCLSHIM_API const char *nclshim_node_value_type(const void *node);

NCLSHIM_API const char *nclshim_node_mapping(const void *node);

NCLSHIM_API const char *nclshim_node_source(const void *node);

NCLSHIM_API const char *nclshim_node_version(const void *node);

NCLSHIM_API const char *nclshim_node_guid(const void *node);

NCLSHIM_API int nclshim_node_settable(const void *node);

/** 数据项的当前值（借用；模型里没有值就是 NULL）。 */
NCLSHIM_API const void *nclshim_node_value(const void *node);

/** 采样通道：周期（毫秒）与采样项。 */
NCLSHIM_API int nclshim_node_is_sample_channel(const void *node);

NCLSHIM_API long long nclshim_node_sample_interval(const void *node);

NCLSHIM_API long long nclshim_node_upload_interval(const void *node);

NCLSHIM_API int nclshim_node_sample_item_count(const void *node);

/** 采样项声明的路径（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_node_sample_item_path(const void *node, int index);

/** 子节点遍历：kind 1=device 2=component 3=config 0=dataItem。 */
NCLSHIM_API int nclshim_node_count(const void *node, int kind);

NCLSHIM_API const void *nclshim_node_at(const void *node, int kind, int index);

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
                                              int payload_len);

/** 释放 nclshim_message_parse() 返回的报文（借用的句柄不要传进来）。 */
NCLSHIM_API void nclshim_message_free(const void *msg);

/** 整条报文的 JSON 文本（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_message_write(const void *msg);

NCLSHIM_API int nclshim_message_type(const void *msg);

/** 采样报文：行数 = 数据最多的那一列的点数。 */
NCLSHIM_API int nclshim_sample_rows(const void *msg);

/** 采样报文：列数（= 表头项数）。 */
NCLSHIM_API int nclshim_sample_columns(const void *msg);

NCLSHIM_API int nclshim_sample_is_complete(const void *msg);

NCLSHIM_API const char *nclshim_sample_id(const void *msg);

NCLSHIM_API const char *nclshim_sample_begin_time(const void *msg);

NCLSHIM_API long long nclshim_sample_interval(const void *msg);

NCLSHIM_API long long nclshim_sample_upload_interval(const void *msg);

/** 表头第 col 项（借用）。 */
NCLSHIM_API const char *nclshim_sample_path(const void *msg, int col);

/** 表头拼成一行（malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_sample_header(const void *msg, const char *separator);

/** 某列的形态：槽位数、总点数、是否批量列、编码。 */
NCLSHIM_API int nclshim_sample_column_slots(const void *msg, int col);

NCLSHIM_API int nclshim_sample_column_points(const void *msg, int col);

NCLSHIM_API int nclshim_sample_column_nested(const void *msg, int col);

NCLSHIM_API const char *nclshim_sample_column_encoding(const void *msg, int col);

/** 按行读：第 row 行、第 col 列（借用；越界返回 NULL）。 */
NCLSHIM_API const void *nclshim_sample_value_at(const void *msg, int row, int col);

/** 列内扁平取值（借用）：只按该列自己的顺序，不跨列对齐。 */
NCLSHIM_API const void *nclshim_sample_column_value_at(const void *msg, int col,
                                                       int index);

/** 事件报文：id / time / key / value。 */
NCLSHIM_API const char *nclshim_event_id(const void *msg);

NCLSHIM_API const char *nclshim_event_time(const void *msg);

NCLSHIM_API const char *nclshim_event_key(const void *msg);

NCLSHIM_API const void *nclshim_event_value(const void *msg);

/** 进程级初始化：连 broker（用户名/密码可为 NULL）。 */
NCLSHIM_API int nclshim_open(const char *uri, const char *user, const char *pass);

/** 关掉连接与所有客户端。 */
NCLSHIM_API void nclshim_close(void);

NCLSHIM_API int nclshim_is_open(void);

/** 取（必要时创建）某个 SN 的设备客户端；借用，nclshim_close 后失效。 */
NCLSHIM_API const void *nclshim_client_get(const char *sn);

/** probe：拿模型。模型所有权交给调用方（nclshim_model_free）。 */
NCLSHIM_API int nclshim_client_probe(const void *client, unsigned timeout_ms,
                                     const void **out_model);

/** getValue(path, timeout)：值所有权交给调用方（nclshim_json_free）。 */
NCLSHIM_API int nclshim_client_get_value(const void *client, const char *path,
                                         unsigned timeout_ms, const void **out_json);

/**
 * 把设备模型交给客户端（路径 ↔ id 互查、采样报文按模型补齐缺的 paths 都靠它）。
 *
 * 所有权：这里**先克隆一份**再交给客户端（客户端自己释放），所以调用方手里那份
 * 模型还是自己的，照常 nclshim_model_free —— 不用为"谁 free"扯皮。root 传 NULL
 * 就是清掉客户端当前装载的模型。
 */
NCLSHIM_API int nclshim_client_set_root_node(const void *client, const void *root);

/** getValueRange(path, start, end, timeout)。 */
NCLSHIM_API int nclshim_client_get_value_range(const void *client, const char *path,
                                               int start, int end,
                                               unsigned timeout_ms,
                                               const void **out_json);

/** getLength(path, timeout)。 */
NCLSHIM_API int nclshim_client_get_length(const void *client, const char *path,
                                          unsigned timeout_ms, long long *out_length);

/** setValue(path, value, timeout)；value 是 JSON 文本。 */
NCLSHIM_API int nclshim_client_set_value(const void *client, const char *path,
                                         const char *value_json,
                                         unsigned timeout_ms);

/** setValue(path, value, index, timeout)。 */
NCLSHIM_API int nclshim_client_set_value_index(const void *client, const char *path,
                                               const char *value_json, int index,
                                               unsigned timeout_ms);

/**
 * 方法调用。method 形如 "/plc/setValue"（也接受 "plc/setValue"），params_json 可为
 * NULL；check=1 表示只校验参数、不执行。成功后 *out_json 收到**应答报文**的 JSON
 * 文本（code / params / data / reason），调用方用 nclshim_free 释放。
 */
NCLSHIM_API int nclshim_client_method_call(const void *client, const char *method,
                                           const char *params_json, int check,
                                           unsigned timeout_ms, char **out_json);

/** ping。 */
NCLSHIM_API int nclshim_client_ping(const void *client, unsigned timeout_ms);

/** 路径 ↔ 节点 id 互查（都是 malloc，调用方释放）。 */
NCLSHIM_API char *nclshim_client_get_id(const void *client, const char *path);

NCLSHIM_API char *nclshim_client_get_path(const void *client, const char *id);

/** 订阅采样；msg 只在回调期间有效。 */
NCLSHIM_API int nclshim_client_subscribe_samples(const void *client, int qos,
                                                 void *host);

NCLSHIM_API int nclshim_client_unsubscribe_samples(const void *client);

NCLSHIM_API int nclshim_client_subscribe_events(const void *client, int qos,
                                                void *host);

NCLSHIM_API int nclshim_client_unsubscribe_events(const void *client);

NCLSHIM_API int nclshim_client_sample_count(const void *client);

NCLSHIM_API int nclshim_client_event_count(const void *client);

/** 运行时加/删采样通道：config_json 是一个 SAMPLE_CHANNEL 配置节点。 */
NCLSHIM_API int nclshim_client_add_sample(const void *client, const char *config_json,
                                          unsigned timeout_ms);

NCLSHIM_API int nclshim_client_remove_sample(const void *client, const char *id,
                                             unsigned timeout_ms);

/**
 * 建一个设备端：模型默认走内置模型；broker_url 为空则**不接 MQTT**（离线使用
 * nclshim_server_dispatch() 驱动，或者自己给 publish_host 当传输）；
 * publish_host 非空的另一种用法是"自研传输"：每条出站报文都交给托管侧。
 *
 * 用户名/密码可为 NULL（匿名）。失败返回 NULL。
 */
NCLSHIM_API const void *nclshim_server_create(const char *sn, const char *model_json,
                                              const char *broker_url,
                                              const char *username,
                                              const char *password,
                                              void *publish_host);

/** 释放设备端：先停服务，再拆连接，最后放工具上下文。 */
NCLSHIM_API void nclshim_server_free(const void *handle);

NCLSHIM_API const char *nclshim_server_sn(const void *handle);

/** 设备模型（借用句柄，nclshim_node_* 都能用；服务器释放即失效）。 */
NCLSHIM_API const void *nclshim_server_model(const void *handle);

NCLSHIM_API char *nclshim_server_model_json(const void *handle);

NCLSHIM_API int nclshim_server_binding_count(const void *handle);

NCLSHIM_API int nclshim_server_operation_count(const void *handle);

NCLSHIM_API int nclshim_server_sample_count(const void *handle);

NCLSHIM_API int nclshim_server_sample_upload_count(const void *handle);

NCLSHIM_API int nclshim_server_event_count(const void *handle);

NCLSHIM_API char *nclshim_server_openapi_json(const void *handle, const char *base_url);

NCLSHIM_API int nclshim_server_subscribe(const void *handle);

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
                                             const char *bindings_json, void *host);

NCLSHIM_API int nclshim_server_register_builtin_tool(const void *handle);

NCLSHIM_API int nclshim_server_register_file_tool(const void *handle);

NCLSHIM_API int nclshim_server_start_ftp(const void *handle);

/**
 * 离线驱动一条请求：payload 是原始报文体（不必 NUL 结尾），返回应答报文的 JSON
 * 文本（malloc，调用方释放）。**不经过 MQTT**，也不发布应答 —— 单测与"自己当
 * 传输"的宿主用它。
 */
NCLSHIM_API int nclshim_server_dispatch(const void *handle, const char *topic,
                                        const void *payload, int payload_len,
                                        char **out_json);

NCLSHIM_API int nclshim_server_invoke_method_call(const void *handle,
                                                  const char *method,
                                                  const char *params_json,
                                                  char **out_json);

NCLSHIM_API int nclshim_server_check_method_call(const void *handle,
                                                 const char *method,
                                                 const char *params_json,
                                                 char **out_json);

NCLSHIM_API int nclshim_server_init_samples(const void *handle);

/** 运行时加一个采样通道（config_json 是一个 SAMPLE_CHANNEL 配置节点）。 */
NCLSHIM_API int nclshim_server_add_sample(const void *handle, const char *config_json);

NCLSHIM_API int nclshim_server_remove_sample(const void *handle, const char *id);

NCLSHIM_API void nclshim_server_stop_all_samples(const void *handle);

/** 推一条事件到 Event/<sn>；event_json 形如 {"key":...,"value":...}。 */
NCLSHIM_API int nclshim_server_push_event(const void *handle, const char *event_id,
                                          const char *event_json);

NCLSHIM_API int nclshim_server_push_event_ex(const void *handle, const char *event_id,
                                             const char *event_json,
                                             long long time_ms,
                                             const char *message_id);

/**
 * 复制一份文本（malloc，调用方 nclshim_free）。**回调里回填字符串必须用它**：
 * 垫片随后会用自己这边的 free() 释放，托管侧自己 malloc/python bytes/GC 出来的
 * 内存不能这么用（跨 CRT / 跨分配器）。
 */
NCLSHIM_API char *nclshim_strdup(const char *text);


#ifdef __cplusplus
}
#endif

#endif /* NCLINK_SHIM_H */
