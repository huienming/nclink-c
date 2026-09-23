// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.Map;

/** Java 对象 -> JSON 文本（设备端应答值用；字符串按 JSON 文本原样放行）。 */
final class JsonValues {
    private JsonValues() {
    }

    static String toJson(Object value) {
        StringBuilder out = new StringBuilder();
        write(out, value);
        return out.toString();
    }

    /** 一定加引号的 JSON 字符串（方法名、路径这类字面量用它）。 */
    static String quoted(String text) {
        StringBuilder out = new StringBuilder();
        quote(out, text == null ? "" : text);
        return out.toString();
    }

    private static void write(StringBuilder out, Object value) {
        if (value == null) {
            out.append("null");
        } else if (value instanceof Json) {
            out.append(((Json) value).encode());
        } else if (value instanceof String) {
            out.append((String) value);                 // 已经是 JSON 文本
        } else if (value instanceof Boolean || value instanceof Number) {
            out.append(value.toString());
        } else if (value instanceof CharSequence || value instanceof Character) {
            quote(out, value.toString());
        } else if (value instanceof Map) {
            out.append('{');
            boolean first = true;
            for (Object entry : ((Map<?, ?>) value).entrySet()) {
                Map.Entry<?, ?> item = (Map.Entry<?, ?>) entry;
                if (!first) {
                    out.append(',');
                }
                first = false;
                quote(out, String.valueOf(item.getKey()));
                out.append(':');
                write(out, item.getValue());
            }
            out.append('}');
        } else if (value instanceof Iterable) {
            out.append('[');
            boolean first = true;
            for (Object item : (Iterable<?>) value) {
                if (!first) {
                    out.append(',');
                }
                first = false;
                write(out, item);
            }
            out.append(']');
        } else if (value.getClass().isArray()) {
            out.append('[');
            int length = java.lang.reflect.Array.getLength(value);
            for (int i = 0; i < length; i++) {
                if (i > 0) {
                    out.append(',');
                }
                write(out, java.lang.reflect.Array.get(value, i));
            }
            out.append(']');
        } else {
            quote(out, value.toString());
        }
    }

    private static void quote(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"':
                    out.append("\\\"");
                    break;
                case '\\':
                    out.append("\\\\");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c);
                    }
            }
        }
        out.append('"');
    }
}
