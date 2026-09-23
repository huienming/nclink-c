// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 库返回的非 0 错误码。
 *
 * <p>{@link #code()} 是 C 的 ncl_err，{@link #name()} 是它的英文名，
 * {@link #op()} 是出错的那个调用（probe / getValue ...）。
 */
public class NclinkException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    private final int code;
    private final String op;
    private final String name;

    public NclinkException(int code, String op) {
        this(code, op, null);
    }

    public NclinkException(int code, String op, String detail) {
        super(message(code, op, detail));
        this.code = code;
        this.op = op;
        this.name = errName(code);
    }

    private static String message(int code, String op, String detail) {
        String base = (op == null || op.isEmpty()) ? errName(code) : op + ": " + errName(code);
        return detail == null ? base : base + "（" + detail + "）";
    }

    private static String errName(int code) {
        String name = Native.errName(code);
        return name != null ? name : ("err-" + code);
    }

    public int code() {
        return code;
    }

    public String op() {
        return op;
    }

    public String name() {
        return name;
    }

    /** rc == 0 直接返回，否则抛异常。 */
    static void check(int rc, String op) {
        if (rc != 0) {
            throw new NclinkException(rc, op);
        }
    }
}
