// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** 日志级别（对应 C 的 ncl_log_level）。 */
public enum LogLevel {
    DEBUG(0),
    INFO(1),
    WARN(2),
    ERROR(3),
    FATAL(4);

    private final int code;

    LogLevel(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }
}
