/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - MQTT topic builders.
 *
 * Every builder returns a freshly allocated string that the caller frees with
 * free(). NULL is returned on allocation failure.
 *
 * Most topics take an optional client id: pass NULL for the single-segment
 * form.
 */
#ifndef NCL_TOPIC_H
#define NCL_TOPIC_H

#ifdef __cplusplus
extern "C" {
#endif

#define NCL_TOPIC_PING_PREFIX                "Ping/"
#define NCL_TOPIC_PONG_PREFIX                "Pong/"
#define NCL_TOPIC_PROBE_QUERY_REQUEST_PREFIX "Probe/Query/Request/"
#define NCL_TOPIC_PROBE_QUERY_RESPONSE_PREFIX "Probe/Query/Response/"
#define NCL_TOPIC_PROBE_SET_REQUEST_PREFIX   "Probe/Set/Request/"
#define NCL_TOPIC_PROBE_SET_RESPONSE_PREFIX  "Probe/Set/Response/"
#define NCL_TOPIC_QUERY_REQUEST_PREFIX       "Query/Request/"
#define NCL_TOPIC_QUERY_RESPONSE_PREFIX      "Query/Response/"
#define NCL_TOPIC_SET_REQUEST_PREFIX         "Set/Request/"
#define NCL_TOPIC_SET_RESPONSE_PREFIX        "Set/Response/"
#define NCL_TOPIC_SAMPLE_PREFIX              "Sample/"
#define NCL_TOPIC_REGISTER_REQUEST           "Register/Request"
#define NCL_TOPIC_PROBE_VERSION_PREFIX       "Probe/Version/"
#define NCL_TOPIC_METHOD_CALL_REQUEST_PREFIX "Method/Call/Request/"
#define NCL_TOPIC_METHOD_CALL_RESPONSE_PREFIX "Method/Call/Response/"
/* 异步方法调用：状态与结果各一对，方向都是客户端 → 设备（Request）/ 设备 → 客户端
 * （Response），用 handler（方法句柄，标识本次调用的线程对象）关联。 */
#define NCL_TOPIC_METHOD_STATUS_REQUEST_PREFIX  "Method/Status/Request/"
#define NCL_TOPIC_METHOD_STATUS_RESPONSE_PREFIX "Method/Status/Response/"
#define NCL_TOPIC_METHOD_RESULT_REQUEST_PREFIX  "Method/Result/Request/"
#define NCL_TOPIC_METHOD_RESULT_RESPONSE_PREFIX "Method/Result/Response/"
#define NCL_TOPIC_EVENT_PREFIX               "Event/"

/** Build "<prefix><deviceId>[/<clientId>]". */
char *ncl_topic_build(const char *prefix, const char *device_id,
                      const char *client_id);

char *ncl_topic_ping(const char *sn);
char *ncl_topic_pong(const char *sn);

char *ncl_topic_probe_query_request(const char *device_id, const char *client_id);
char *ncl_topic_probe_query_response(const char *device_id, const char *client_id);
char *ncl_topic_probe_set_request(const char *device_id, const char *client_id);
char *ncl_topic_probe_set_response(const char *device_id, const char *client_id);

char *ncl_topic_query_request(const char *device_id, const char *client_id);
char *ncl_topic_query_response(const char *device_id, const char *client_id);
char *ncl_topic_set_request(const char *device_id, const char *client_id);
char *ncl_topic_set_response(const char *device_id, const char *client_id);

char *ncl_topic_sample(const char *device_id, const char *client_id);
char *ncl_topic_probe_version(const char *device_id, const char *client_id);
char *ncl_topic_method_call_request(const char *device_id, const char *client_id);
char *ncl_topic_method_call_response(const char *device_id, const char *client_id);
char *ncl_topic_method_status_request(const char *device_id, const char *client_id);
char *ncl_topic_method_status_response(const char *device_id, const char *client_id);
char *ncl_topic_method_result_request(const char *device_id, const char *client_id);
char *ncl_topic_method_result_response(const char *device_id, const char *client_id);
char *ncl_topic_event(const char *device_id, const char *client_id);


char *ncl_topic_register_request(void);

/**
 * Extract the device serial number from an inbound topic: split on '/', and for
 * "Sample/..." topics the serial number is the second-to-last segment,
 * otherwise the last one.
 * Returns a heap string, or NULL when the topic has no segment.
 */
char *ncl_topic_extract_sn(const char *topic);

#ifdef __cplusplus
}
#endif

#endif /* NCL_TOPIC_H */
