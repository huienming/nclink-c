// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** ncl_operation：路径绑定里的操作（"<operation>#<path>"）。 */
public enum Operation {
    GET_VALUE(0),
    GET_LENGTH(1),
    GET_KEYS(2),
    GET_ATTRIBUTES(3),
    SET_VALUE(4),
    ADD(5),
    DELETE(6),
    FUNC_CALL(7),
    FUNC_STATUS(8),
    FUNC_RESULT(9),
    FUNC_CANCEL(10);

    private final int code;

    Operation(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }
}