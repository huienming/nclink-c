// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** ncl_json_type。 */
public enum JsonType {
    NULL(0),
    BOOL(1),
    NUMBER(2),
    STRING(3),
    ARRAY(4),
    OBJECT(5);

    private final int code;

    JsonType(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static JsonType of(int code) {
        for (JsonType type : values()) {
            if (type.code == code) {
                return type;
            }
        }
        return NULL;
    }
}
