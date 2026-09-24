// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** 既不是采样也不是事件的报文：留类型与 JSON 文本。 */
public final class Message {
    private final String topic;
    private final MessageType type;
    private final String rawJson;

    Message(String topic, MessageType type, String rawJson) {
        this.topic = topic;
        this.type = type;
        this.rawJson = rawJson;
    }

    public String topic() {
        return topic;
    }

    public MessageType type() {
        return type;
    }

    public String rawJson() {
        return rawJson;
    }

    /** 报文 JSON -> Java 原生对象。 */
    public Object toJavaObject() {
        Json parsed = Json.parse(rawJson);
        try {
            return parsed.toJavaObject();
        } finally {
            parsed.close();
        }
    }

    @Override
    public String toString() {
        return "Message " + topic + " type=" + type;
    }
}
