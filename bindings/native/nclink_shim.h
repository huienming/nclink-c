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

/**
 * 同 nclshim_open()，但可以带上 TLS 选项（只在"库编了 TLS"时有意义）：
 * ca_file / client_cert / client_key / server_name 可为 NULL（用默认），
 * verify_peer 传 -1 表示不覆盖（默认校验）。
 */
NCLSHIM_API int nclshim_open_ex(const char *uri, const char *user, const char *pass,
                                const char *ca_file, const char *client_cert,
                                const char *client_key, const char *server_name,
                                int verify_peer);

/** 这个垫片链的库有没有编 TLS（1 = ssl:// 可用）。 */
NCLSHIM_API int nclshim_tls_available(void);

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
                                                 void *publish_host);

/** 老入口：不带 TLS（等价于 nclshim_server_create_ex(..., NULL, NULL, NULL, NULL, -1, ...)）。 */
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

/**
 * 起一个 HTTP 端点：port=0 用随机端口（端口用 nclshim_http_port 查）。
 * server 非空时挂 REST（OpenAPI + swagger-ui + 工具端点）；with_config 非 0 再
 * 挂配置端点（SN/模型/驱动/mqtt.cfg/服务器列表）。失败返回 NULL。
 */
NCLSHIM_API const void *nclshim_http_start(unsigned port, const void *server,
                                           int with_config);

NCLSHIM_API void nclshim_http_free(const void *handle);

NCLSHIM_API int nclshim_http_port(const void *handle);

NCLSHIM_API int nclshim_http_request_count(const void *handle);

NCLSHIM_API void nclshim_http_set_cors(const void *handle, int enabled);

/** 挂一条自己的路由：method 支持 "*"（见 ncl_http_server_route 的匹配规则）。 */
NCLSHIM_API int nclshim_http_route(const void *handle, const char *method,
                                   const char *path, void *host);

/** 起进程级 FTP 端点（幂等；nclshim_open 也会起）。 */
NCLSHIM_API int nclshim_file_start_ftp(void);

/**
 * 同上，但可以换端口 / 根目录 / 账号：port=0、root/user/password 为 NULL 就是默认
 * （2323、安装根、admin / 123456）。托管侧与 broker 不在同一台机器、或者 2323 被占
 * 的时候用它。
 */
NCLSHIM_API int nclshim_file_start_ftp_ex(unsigned port, const char *root,
                                          const char *user, const char *password);

/**
 * 覆盖设备端文件通道对端的 FTP 端点（默认按 conf/mqtt.cfg 推：broker 的主机名 +
 * 2323 + admin/123456）。host 必填；port=0 / user / password 为 NULL 用默认。
 */
NCLSHIM_API int nclshim_server_set_file_peer(const void *handle, const char *host,
                                             unsigned port, const char *user,
                                             const char *password);

NCLSHIM_API void nclshim_file_stop_ftp(void);

/**
 * 上传一个文件：local_file_path 是**相对路径**（形如 "/demo.txt"），文件必须在
 * <root>/<sn><相对路径> 上（与 C API 一致）。
 */
NCLSHIM_API int nclshim_client_file_write(const void *client,
                                          const char *local_file_path);

/** 下载一个文件；返回落盘后的本地绝对路径（malloc，调用方 nclshim_free）。 */
NCLSHIM_API char *nclshim_client_file_read(const void *client,
                                           const char *remote_file_path);

/** 列目录：返回属性数组的 JSON 文本（malloc）；失败返回 NULL。 */
NCLSHIM_API char *nclshim_client_file_ll_json(const void *client,
                                              const char *remote_dir);

NCLSHIM_API int nclshim_client_file_mkdir(const void *client, const char *remote_dir);

NCLSHIM_API int nclshim_client_file_delete(const void *client,
                                           const char *remote_file_path);

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
                                                char **out_json);

NCLSHIM_API int nclshim_file_need_compression(const char *file_name);

NCLSHIM_API int nclshim_file_total_chunks(long long size);

/** 文件内容的 SHA-256（小写十六进制，malloc；调用方 nclshim_free）。 */
NCLSHIM_API char *nclshim_file_checksum(const char *path);

/** 本地文件的属性（一个 JSON 对象；字段顺序按规范固定）：见 ncl_file_attribute_to_json。 */
NCLSHIM_API char *nclshim_file_attribute_json(const char *path, const char *parent);


#ifdef __cplusplus
}
#endif

#endif /* NCLINK_SHIM_H */
