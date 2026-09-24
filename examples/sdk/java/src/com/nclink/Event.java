// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** 一条事件报文（快照，出了回调照样能用）。 */
public final class Event {
    private final String topic;
    private final String id;
    private final String time;
    private final String key;
    private final Object value;
    private final String rawJson;

    Event(String topic, String id, String time, String key, Object value, String rawJson) {
        this.topic = topic;
        this.id = id;
        this.time = time;
        this.key = key;
        this.value = value;
        this.rawJson = rawJson;
    }

    static Event capture(String topic, long msg) {
        long value = Native.eventValue(msg);
        return new Event(topic, Native.eventId(msg), Native.eventTime(msg),
                Native.eventKey(msg),
                value == 0 ? null : Json.borrowed(value).toJavaObject(),
                Native.messageWrite(msg));
    }

    public String topic() {
        return topic;
    }

    public String id() {
        return id;
    }

    public String time() {
        return time;
    }

    public String key() {
        return key;
    }

    public Object value() {
        return value;
    }

    public String rawJson() {
        return rawJson;
    }

    @Override
    public String toString() {
        return "Event " + topic + " id=" + id + " key=" + key + " value=" + value;
    }
}
