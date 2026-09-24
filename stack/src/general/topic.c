/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - topic builders. */
#include "nclink/ncl_topic.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_common.h"

char *ncl_topic_build(const char *prefix, const char *device_id,
                      const char *client_id)
{
    char *out = NULL;
    if (prefix == NULL) {
        prefix = "";
    }
    if (device_id == NULL) {
        device_id = "";
    }
    if (client_id == NULL) {
        ncl_asprintf(&out, "%s%s", prefix, device_id);
    } else {
        ncl_asprintf(&out, "%s%s/%s", prefix, device_id, client_id);
    }
    return out;
}

char *ncl_topic_ping(const char *sn)
{
    return ncl_topic_build(NCL_TOPIC_PING_PREFIX, sn, NULL);
}

char *ncl_topic_pong(const char *sn)
{
    return ncl_topic_build(NCL_TOPIC_PONG_PREFIX, sn, NULL);
}

char *ncl_topic_probe_query_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_PROBE_QUERY_REQUEST_PREFIX, device_id, client_id);
}

char *ncl_topic_probe_query_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_PROBE_QUERY_RESPONSE_PREFIX, device_id, client_id);
}

char *ncl_topic_probe_set_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_PROBE_SET_REQUEST_PREFIX, device_id, client_id);
}

char *ncl_topic_probe_set_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_PROBE_SET_RESPONSE_PREFIX, device_id, client_id);
}

char *ncl_topic_query_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_QUERY_REQUEST_PREFIX, device_id, client_id);
}

char *ncl_topic_query_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_QUERY_RESPONSE_PREFIX, device_id, client_id);
}

char *ncl_topic_set_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_SET_REQUEST_PREFIX, device_id, client_id);
}

char *ncl_topic_set_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_SET_RESPONSE_PREFIX, device_id, client_id);
}

char *ncl_topic_sample(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_SAMPLE_PREFIX, device_id, client_id);
}

char *ncl_topic_probe_version(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_PROBE_VERSION_PREFIX, device_id, client_id);
}

char *ncl_topic_method_call_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_METHOD_CALL_REQUEST_PREFIX, device_id, client_id);
}

char *ncl_topic_method_call_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_METHOD_CALL_RESPONSE_PREFIX, device_id, client_id);
}

char *ncl_topic_event(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_EVENT_PREFIX, device_id, client_id);
}

char *ncl_topic_method_status_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_METHOD_STATUS_REQUEST_PREFIX, device_id,
                           client_id);
}

char *ncl_topic_method_status_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_METHOD_STATUS_RESPONSE_PREFIX, device_id,
                           client_id);
}

char *ncl_topic_method_result_request(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_METHOD_RESULT_REQUEST_PREFIX, device_id,
                           client_id);
}

char *ncl_topic_method_result_response(const char *device_id, const char *client_id)
{
    return ncl_topic_build(NCL_TOPIC_METHOD_RESULT_RESPONSE_PREFIX, device_id,
                           client_id);
}

char *ncl_topic_register_request(void)
{
    return ncl_strdup(NCL_TOPIC_REGISTER_REQUEST);
}

char *ncl_topic_extract_sn(const char *topic)
{
    const char *end;
    const char *last;
    size_t last_len;

    if (topic == NULL) {
        return NULL;
    }

    /* Drop trailing separators, then isolate the final segment. */
    end = topic + strlen(topic);
    while (end > topic && end[-1] == '/') {
        end--;
    }
    if (end == topic) {
        return NULL;
    }
    last = end;
    while (last > topic && last[-1] != '/') {
        last--;
    }
    last_len = (size_t)(end - last);

    /* "Sample/<path...>/<sn>" carries the serial number before the last
     * segment; every other topic ends with the serial number. */
    if (ncl_str_starts_with(topic, NCL_TOPIC_SAMPLE_PREFIX) && last > topic) {
        const char *prev_end = last - 1; /* the separator before the last segment */
        const char *prev;

        while (prev_end > topic && prev_end[-1] == '/') {
            prev_end--;
        }
        if (prev_end > topic) {
            prev = prev_end;
            while (prev > topic && prev[-1] != '/') {
                prev--;
            }
            return ncl_strndup(prev, (size_t)(prev_end - prev));
        }
    }

    return ncl_strndup(last, last_len);
}
