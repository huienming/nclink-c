// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** ncl_msg_type。 */
public enum MessageType {
    UNKNOWN(0),
    PING(1),
    PONG(2),
    PROBE_VERSION(3),
    REGISTER_REQUEST(4),
    REGISTER_RESPONSE(5),
    QUERY_REQUEST(6),
    QUERY_RESPONSE(7),
    SET_REQUEST(8),
    SET_RESPONSE(9),
    PROBE_QUERY_REQUEST(10),
    PROBE_QUERY_RESPONSE(11),
    PROBE_SET_REQUEST(12),
    PROBE_SET_RESPONSE(13),
    SAMPLE(14),
    EVENT(15),
    METHOD_CALL_REQUEST(16),
    METHOD_CALL_RESPONSE(17);

    private final int code;

    MessageType(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static MessageType of(int code) {
        for (MessageType type : values()) {
            if (type.code == code) {
                return type;
            }
        }
        return UNKNOWN;
    }
}
