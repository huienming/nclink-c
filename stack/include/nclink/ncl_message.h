/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - protocol messages.
 *
 * Every message kind (Ping, Probe, Query, Set, MethodCall, Sample, Event and
 * their responses) is one ncl_message with a tagged union payload:
 *   - build one with ncl_message_new() plus the ncl_message_set_*() /
 *     ncl_message_add_*() helpers, then call ncl_message_finalise(), which
 *     fills in a random UUID when "@id" is missing;
 *   - check ncl_message_is_valid() before sending;
 *   - serialise with ncl_message_to_json() / ncl_message_write_string().
 *
 * Serialisation uses the property order of the specification and omits null
 * valued entries.
 */
#ifndef NCL_MESSAGE_H
#define NCL_MESSAGE_H

#include <stdbool.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Message kinds; the order matches the response pairing below. */
typedef enum {
    NCL_MSG_UNKNOWN = 0,
    NCL_MSG_PING,
    NCL_MSG_PONG,
    NCL_MSG_PROBE_VERSION,
    NCL_MSG_REGISTER_REQUEST,
    NCL_MSG_REGISTER_RESPONSE,
    NCL_MSG_QUERY_REQUEST,
    NCL_MSG_QUERY_RESPONSE,
    NCL_MSG_SET_REQUEST,
    NCL_MSG_SET_RESPONSE,
    NCL_MSG_PROBE_QUERY_REQUEST,
    NCL_MSG_PROBE_QUERY_RESPONSE,
    NCL_MSG_PROBE_SET_REQUEST,
    NCL_MSG_PROBE_SET_RESPONSE,
    NCL_MSG_SAMPLE,
    NCL_MSG_EVENT,
    NCL_MSG_METHOD_CALL_REQUEST,
    NCL_MSG_METHOD_CALL_RESPONSE,
    /* Asynchronous method call: status / result pairs (Method/Status|Result). */
    NCL_MSG_METHOD_STATUS_REQUEST,
    NCL_MSG_METHOD_STATUS_RESPONSE,
    NCL_MSG_METHOD_RESULT_REQUEST,
    NCL_MSG_METHOD_RESULT_RESPONSE
} ncl_msg_type;

const char *ncl_msg_type_name(ncl_msg_type type);

/** The message kind implied by a topic prefix (NCL_MSG_UNKNOWN when none). */
ncl_msg_type ncl_msg_type_from_topic(const char *topic);

/* ================================================================ items === */

/** QueryRequestItem: {"id":..,"params":{...}} */
typedef struct {
    char     *id;
    ncl_json *params; /**< owned object, may be NULL */
} ncl_query_request_item;

/** Query response item: {"id","code","reason","params","values":[]} */
typedef struct {
    char     *id;
    char     *code;
    char     *reason;
    ncl_json *params;
    ncl_json *values; /**< owned array, may be NULL */
} ncl_query_response_item;

/** SetRequestItem: {"id":..,"params":{...}} */
typedef struct {
    char     *id;
    ncl_json *params;
} ncl_set_request_item;

/** Set response item: {"id","code","reason","params","result"} */
typedef struct {
    char     *id;
    char     *code;
    char     *reason;
    ncl_json *params;
    ncl_json *result;
} ncl_set_response_item;

/** SampleItem: {"encoding":..,"data":[]} */
typedef struct {
    char     *encoding;
    ncl_json *data; /**< owned array */
} ncl_sample_item;

ncl_query_request_item  *ncl_query_request_item_new(const char *id);
void                     ncl_query_request_item_free(ncl_query_request_item *item);
bool                     ncl_query_request_item_is_valid(const ncl_query_request_item *item);
const char              *ncl_query_request_item_operation(const ncl_query_request_item *item);
/** Expands an "indexes" string ("3" or "1-4") into a flat id list. */
ncl_err                  ncl_query_request_item_indexes(const ncl_query_request_item *item,
                                                        long long **out, size_t *count);

ncl_query_response_item *ncl_query_response_item_new(const char *id);
void                     ncl_query_response_item_free(ncl_query_response_item *item);
bool                     ncl_query_response_item_is_valid(const ncl_query_response_item *item);
const char              *ncl_query_response_item_operation(const ncl_query_response_item *item);
bool                     ncl_query_response_item_has_data(const ncl_query_response_item *item);
ncl_json                *ncl_query_response_item_data(const ncl_query_response_item *item);
ncl_err                  ncl_query_response_item_add_value(ncl_query_response_item *item,
                                                           ncl_json *value);
/** True when the response item answers this request item. */
bool                     ncl_query_response_item_matches(const ncl_query_response_item *item,
                                                         const ncl_query_request_item *request);

ncl_set_request_item    *ncl_set_request_item_new(const char *id);
void                     ncl_set_request_item_free(ncl_set_request_item *item);
bool                     ncl_set_request_item_is_valid(const ncl_set_request_item *item);
const char              *ncl_set_request_item_operation(const ncl_set_request_item *item);

ncl_set_response_item   *ncl_set_response_item_new(const char *id);
void                     ncl_set_response_item_free(ncl_set_response_item *item);
bool                     ncl_set_response_item_is_valid(const ncl_set_response_item *item);
bool                     ncl_set_response_item_matches(const ncl_set_response_item *item,
                                                       const ncl_set_request_item *request);
/**
 * True when the item reports a failure, i.e. when "code" is "NG".
 */
bool                     ncl_set_response_item_has_error(const ncl_set_response_item *item);

ncl_sample_item         *ncl_sample_item_new(void);
ncl_sample_item         *ncl_sample_item_clone(const ncl_sample_item *item);
void                     ncl_sample_item_free(ncl_sample_item *item);
bool                     ncl_sample_item_is_valid(const ncl_sample_item *item);
ncl_err                  ncl_sample_item_add_value(ncl_sample_item *item, ncl_json *value);
bool                     ncl_sample_item_is_same(const ncl_sample_item *a,
                                                 const ncl_sample_item *b);

/* -------------------------------------------------- params helper access -- */

/**
 * Helpers for the derived members shared by the request and response items,
 * e.g. the "offset" of a query request item, which is read from params.offset
 * whether it arrives as a string or as a number.
 */
const char *ncl_params_operation(const ncl_json *params, const char *fallback);
bool        ncl_params_has(const ncl_json *params, const char *key);
const char *ncl_params_string(const ncl_json *params, const char *key);
bool        ncl_params_int(const ncl_json *params, const char *key, long long *out);
ncl_json   *ncl_params_get(const ncl_json *params, const char *key);

/** Sets params[key] = string, creating the params object when needed. */
ncl_err ncl_params_set_string(ncl_json **params, const char *key, const char *value);
ncl_err ncl_params_set_int(ncl_json **params, const char *key, long long value);
/** Sets params[key] = value (ownership transfers). */
ncl_err ncl_params_set(ncl_json **params, const char *key, ncl_json *value);
/** Appends a string to the array stored at params[key]. */
ncl_err ncl_params_append_string(ncl_json **params, const char *key, const char *value);

/** Expands ["3","1-4"] into [3,1,4]. */
ncl_err ncl_params_indexes(const ncl_json *params, long long **out, size_t *count);

/* ============================================================== message === */

typedef struct ncl_message ncl_message;

struct ncl_message {
    ncl_msg_type type;
    char        *message_id; /**< "@id" */

    union {
        struct {
            /**
             * "code": "OK" / "NG". A Pong is a liveness answer and nothing
             * else - the whole document is one small publish (see
             * ncl_server.h: the method metadata travels in the model's
             * METHODS item, the OpenAPI document over REST).
             */
            char *code;
        } pong;
        struct {
            char *version;
        } probe_version;
        struct {
            char *device_id; /**< "deviceid" */
        } register_request;
        struct {
            char *code;
        } register_response;
        struct {
            ncl_ptrvec items; /**< ncl_query_request_item* */
        } query_request;
        struct {
            ncl_ptrvec items; /**< ncl_query_response_item* */
        } query_response;
        struct {
            ncl_ptrvec items; /**< ncl_set_request_item* */
        } set_request;
        struct {
            ncl_ptrvec items; /**< ncl_set_response_item* */
        } set_response;
        struct {
            char     *code;
            ncl_node *model; /**< "probe" */
            char     *reason;
        } probe_query_response;
        struct {
            ncl_node *model; /**< "probe" */
        } probe_set_request;
        struct {
            char *code;
            char *reason;
        } probe_set_response;
        struct {
            ncl_strvec paths; /**< "paths" */
            char      *id;
            char      *begin_time; /**< "beginTime", stored as a string */
            ncl_ptrvec data;       /**< ncl_sample_item* */
            bool       has_interval;
            long long  interval;
            bool       has_upload_interval;
            long long  upload_interval;
        } sample;
        struct {
            char     *id;
            char     *time; /**< milliseconds since the epoch, as a string */
            ncl_json *event;
        } event;
        struct {
            char     *method;
            ncl_json *params;
            bool      check;
            bool      has_check;
            char     *token;
            /* "async": run the method on a worker thread and answer with a
             * handler instead of waiting for the outcome. */
            bool      async;
            bool      has_async;
        } method_call_request;
        struct {
            char     *code;
            char     *method;
            ncl_json *params;
            bool      check;
            bool      has_check;
            char     *token;
            ncl_json *data;
            char     *reason;
            char     *handler; /**< async: handle of the running call */
        } method_call_response;
        /* Method/Status/Request: {"id":..,"handler":..} */
        struct {
            char *id;
            char *handler;
        } method_status_request;
        /* Method/Status/Response: process / status / code */
        struct {
            char     *id;
            char     *handler;
            long long process;
            bool      has_process;
            char     *status;
            char     *code;
        } method_status_response;
        /* Method/Result/Request: {"id":..,"handler":..} */
        struct {
            char *id;
            char *handler;
        } method_result_request;
        /* Method/Result/Response: code / return / result */
        struct {
            char     *id;
            char     *handler;
            char     *code;
            ncl_json *returns; /**< "return": the method's value */
            char     *result;  /**< finished / cancel / error */
        } method_result_response;
    } as;
};

/* Construction ------------------------------------------------------------ */

ncl_message *ncl_message_new(ncl_msg_type type);
void         ncl_message_free(ncl_message *msg);

/** Returns NCL_ERR_INVALID_MESSAGE when the object is not valid, otherwise
 *  fills in a random UUID when "@id" is absent. */
ncl_err ncl_message_finalise(ncl_message *msg);

/** Sets "@id". Passing NULL clears it. */
ncl_err ncl_message_set_message_id(ncl_message *msg, const char *id);

/* Field setters shorthands, one per concrete message type. */
/** Sets "code"; also the status a Pong carries. */
ncl_err ncl_message_set_code(ncl_message *msg, const char *code);
ncl_err ncl_message_set_reason(ncl_message *msg, const char *reason);
ncl_err ncl_message_set_version(ncl_message *msg, const char *version);
ncl_err ncl_message_set_device_id(ncl_message *msg, const char *device_id);
ncl_err ncl_message_set_model(ncl_message *msg, ncl_node *model);
/**
 * Detach the device model carried by a probe message. The caller takes
 * ownership; the message will no longer free it.
 * Returns NULL when the message carries no model.
 */
ncl_node *ncl_message_take_model(ncl_message *msg);
ncl_err ncl_message_set_method(ncl_message *msg, const char *method);
ncl_err ncl_message_set_params(ncl_message *msg, ncl_json *params);
ncl_err ncl_message_set_token(ncl_message *msg, const char *token);
ncl_err ncl_message_set_check(ncl_message *msg, bool check);
ncl_err ncl_message_set_data(ncl_message *msg, ncl_json *data);
ncl_err ncl_message_set_event(ncl_message *msg, ncl_json *event);
ncl_err ncl_message_set_sample_id(ncl_message *msg, const char *id);

/*
 * Asynchronous method call (Method/Call with "async": true, then the
 * Method/Status|Result pairs). The handler is the device side handle of one
 * running call: it is minted by the server and used by the client to address
 * that particular call's thread.
 */
ncl_err     ncl_message_set_handler(ncl_message *msg, const char *handler);
ncl_err     ncl_message_set_request_id(ncl_message *msg, const char *id);
ncl_err     ncl_message_set_async(ncl_message *msg, bool async);
ncl_err     ncl_message_set_status(ncl_message *msg, const char *status);
ncl_err     ncl_message_set_process(ncl_message *msg, long long process);
ncl_err     ncl_message_set_result(ncl_message *msg, const char *result);
/** Method/Result/Response "return"; takes ownership of @p value. */
ncl_err     ncl_message_set_return(ncl_message *msg, ncl_json *value);

const char *ncl_message_handler(const ncl_message *msg);
const char *ncl_message_request_id(const ncl_message *msg);
/** The "code" of a response message (register / probe / method / pong). */
const char *ncl_message_code(const ncl_message *msg);
bool        ncl_message_async(const ncl_message *msg);
bool        ncl_message_has_async(const ncl_message *msg);
const char *ncl_message_status(const ncl_message *msg);
/** True when "process" was present; copies it into @p out. */
bool        ncl_message_process(const ncl_message *msg, long long *out);
const char *ncl_message_result(const ncl_message *msg);
/** Borrowed "return" of a Method/Result/Response. */
ncl_json   *ncl_message_get_return(const ncl_message *msg);
/**
 * Event.time in milliseconds since the epoch. Passing 0 fills it with the
 * current time.
 */
ncl_err ncl_message_set_event_time_ms(ncl_message *msg, int64_t millis);
ncl_err ncl_message_set_begin_time(ncl_message *msg, const char *begin_time);
ncl_err ncl_message_set_sample_interval(ncl_message *msg, long long interval);
ncl_err ncl_message_set_upload_interval(ncl_message *msg, long long interval);
ncl_err ncl_message_add_sample_path(ncl_message *msg, const char *path);
ncl_err ncl_message_add_sample_item(ncl_message *msg, ncl_sample_item *item);

/* Item collection helpers. Each "add" transfers ownership on success and
 * releases the item on failure. */
ncl_err ncl_message_add_query_request_item(ncl_message *msg, ncl_query_request_item *item);
ncl_err ncl_message_add_query_response_item(ncl_message *msg, ncl_query_response_item *item);
ncl_err ncl_message_add_set_request_item(ncl_message *msg, ncl_set_request_item *item);
ncl_err ncl_message_add_set_response_item(ncl_message *msg, ncl_set_response_item *item);

size_t ncl_message_item_count(const ncl_message *msg);
/** Item accessors; the concrete type depends on the message kind. */
void *ncl_message_item_at(const ncl_message *msg, size_t index);

/**
 * "表头" of a Sample message - the list of data items this report collected.
 * On the wire it is an array:
 *
 *     "paths": ["/STATUS", "/AXIS@0/POSITION"]
 *
 * The paths are **device-internal**: the device segment ("/MACHINE") is left
 * out, because every column of one channel sits on the same device and the
 * topic's <sn> already names it (ncl_node_path_in_device() is the rule). The
 * model's own paths, binding keys and REST addresses stay absolute - prefix the
 * header entry with the device path (ncl_node_device_path()) to get there.
 *
 * and the array is directly reachable as `msg->as.sample.paths` (an ncl_strvec,
 * index-aligned with the sample items in `data`). This helper is only for
 * consumers that want one delimited line - logs, CSV headers, table widgets:
 * it joins the same paths with @p separator (NULL means ";"). Returns a heap
 * string, or NULL when @p msg is not a sample message.
 */
char *ncl_message_sample_header(const ncl_message *msg, const char *separator);

/**
 * True when a Sample message is ready to hand to a consumer: it carries a
 * non-empty header (paths), one data block per header entry (same order), the
 * same number of slots in every column, and at least one slot.
 *
 * 只要求**外层（槽位）对齐**，内层不设统一形状：采样率不同的数据项可以放在同
 * 一个通道里，靠"每槽装几个点"体现各自的采样率（1 ms 槽位里功率装 1 个点、
 * 振动装 4 个点 = 0.25 ms）。所以一条报文里可以同时有标量列、批量列，甚至某列
 * 每槽点数不等 —— 消费端按列读（见下面的 _is_nested / _value_count / _value_at
 * 助手），不要按下标跨列对读。
 *
 * 唯一的例外是**整列 `[]`**：那是设备"本周期该项没有数据"的占位，判为还没填完，
 * 交给 ncl_message_sample_normalise() 换成 null 之后再发。
 *
 * The device side checks this before publishing, so a consumer never sees a
 * half-filled report (header without data, or columns of different lengths).
 */
bool ncl_message_sample_is_complete(const ncl_message *msg);

/**
 * 用设备模型把设备端"省掉/占位"的采样信息补回规范形状，好让消费端继续按行
 * 列对读。做两件事，且只在能确定映射时动手：
 *
 *   1. 表头：`paths` 缺失或条数与 data 列数不符时，拿 `msg->as.sample.id`
 *      （即 "Sample/<sn>/<通道id>" 里的通道 id）到模型里找同名 SAMPLE_CHANNEL，
 *      按它声明的采样项顺序补出表头。模型的项数必须与 data 列数一致，否则
 *      无从对应，报文原样返回。
 *   2. 空列：整列都是空数组（设备用 `[]` 表示"本周期该项没有数据"）时，把该列
 *      换成等长的 null。按列处理，不看别的列是什么形状。
 *
 * 补不了就绝不猜：通道查不到、模型项数对不上，一律保持原样，让
 * ncl_message_sample_is_complete() 继续如实报"不完整"。
 *
 * 返回 NCL_OK 表示补完后 ncl_message_sample_is_complete() 为真；
 * NCL_ERR_INVALID_ARG 表示 @p msg 不是 Sample 报文；NCL_ERR_NOT_FOUND 表示
 * 模型里没有能对上号、项数一致的采集通道；NCL_ERR_STATE 表示映射补上了、但
 * 报文仍不满足完整性（表头与列数不一致、各列槽位数不齐等）。
 */
ncl_err ncl_message_sample_normalise(ncl_message *msg, ncl_node *root);

/**
 * 一列的元素可能是标量，也可能是"一个采样槽位内的一批值"，甚至两者混着来。
 * 这几个助手让消费端不必自己判断两层结构：
 *
 *   sampleInterval = 1ms, uploadInterval = 200ms，工具每次返回 10 个值
 *   → 一条报文 200 个槽位，每槽一个 10 元素数组（合计 2000 点）
 *
 *   同一条报文里另一列每槽只装 1 个点（标量）也完全合法：各列按自己的采样率走。
 */

/** 该列是否含数组元素（即是否是亚毫秒批量采样）。 */
bool ncl_sample_item_is_nested(const ncl_sample_item *item);

/** 该列的总点数：标量（含 null）算 1，数组算其长度。 */
size_t ncl_sample_item_value_count(const ncl_sample_item *item);

/**
 * 按"扁平下标"取第 @p index 个点：标量列等同于下标取值，数组列按槽位顺序展开。
 * 返回借用指针，越界返回 NULL。
 */
const ncl_json *ncl_sample_item_value_at(const ncl_sample_item *item,
                                         size_t index);

/**
 * 行数（= 整个报文的点数）：**数据最多的那一列**的点数。
 *
 * 各列采样率不同时（例如功率每槽 1 点、振动每槽 4 点），最细的那一列就是行轴：
 * 行数 = 它的点数；其余列按 ncl_message_sample_value_at() 的"覆盖该行的第一个
 * 点"补齐，于是按行遍历就能把不同采样率的列放在一张表里读。
 */
size_t ncl_message_sample_point_count(const ncl_message *msg);

/**
 * 按行读某一列的值：@p row 取 [0, ncl_message_sample_point_count())，@p column 取
 * [0, 列数)。
 *
 * 行轴是最细的那一列（点数最多的），所以：
 *
 *   - 最多的那一列：逐点展开，第 row 行就是它的第 row 个点；
 *   - 更粗的列：落在同一段里就**反复取该段的第一个点** —— 1 ms 一列配 0.25 ms
 *     一列时，4 行对应同一段，读到的都是那段（那个槽位）里覆盖该行的第一个点。
 *
 * 返回借用指针（该列已按 ncl_sample_item_value_at() 摊平）；越界或该列没有点时
 * 返回 NULL。
 */
const ncl_json *ncl_message_sample_value_at(const ncl_message *msg, size_t row,
                                            size_t column);

/* Validation and matching ------------------------------------------------ */

/** True when the message satisfies the rules of its kind. */
bool ncl_message_is_valid(const ncl_message *msg);

/** True when @p response answers every item of @p request. */
bool ncl_message_matches(const ncl_message *response, const ncl_message *request);

/** Whether a query response carries a "data" member, and that member. */
bool      ncl_message_has_data(const ncl_message *msg);
ncl_json *ncl_message_get_data(const ncl_message *msg);

/* Serialisation ---------------------------------------------------------- */

/** Serialise using the property order of the specification (null entries are
 *  omitted). */
ncl_json *ncl_message_to_json(const ncl_message *msg);
char     *ncl_message_write_string(const ncl_message *msg);

/** Build a message of @p type from its JSON form. Returns NULL when the input
 *  is malformed or does not match @p type. */
ncl_message *ncl_message_from_json(ncl_msg_type type, const ncl_json *json);

/** Convenience: infer the type from @p topic then parse @p payload. */
ncl_message *ncl_message_parse(const char *topic, const char *payload,
                               size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MESSAGE_H */
