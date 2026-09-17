// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 一个 JSON 值（对象、数组、标量都算）。
 *
 * <p>Java 侧不做 JSON 解析：文本进 {@link #parse(String)}，值出来走库自己的解析
 * 器；要 Java 原生对象就 {@link #toJavaObject()}。
 *
 * <p>所有权：
 *
 * <ul>
 *   <li><b>自有</b>（{@code parse} / {@code clone} / 各种 {@code getXxx} 的返回
 *       值）：用完 {@link #close()} 或 try-with-resources；
 *   <li><b>借用</b>视图（{@code get(int)} / {@code get(String)} / {@code items()}）：
 *       不用关，它们持有宿主引用，宿主活着就有效。
 * </ul>
 */
public final class Json implements AutoCloseable {
    private long handle;
    private final boolean owned;
    private final Object host;      // 借用视图：钉住宿主，句柄才有效

    private Json(long handle, boolean owned, Object host) {
        this.handle = handle;
        this.owned = owned;
        this.host = host;
    }

    static Json owned(long handle) {
        return new Json(handle, true, null);
    }

    static Json view(long handle, Object host) {
        return new Json(handle, false, host);
    }

    /** 借用视图：调用方保证句柄在视图用完之前有效（回调里的报文就是这种）。 */
    static Json borrowed(long handle) {
        return new Json(handle, false, null);
    }

    // ------------------------------------------------------------ 构造 -- //

    /** 解析 JSON 文本；不合法抛 {@link NclinkException}。 */
    public static Json parse(String text) {
        long handle = Native.jsonParse(text);
        if (handle == 0) {
            throw new NclinkException(-3, "Json.parse", "JSON 文本不合法");
        }
        return owned(handle);
    }

    /** 深拷贝。 */
    public Json clone() {
        long copy = Native.jsonClone(requireOpen());
        if (copy == 0) {
            throw new NclinkException(-2, "Json.clone", "内存不足");
        }
        return owned(copy);
    }

    @Override
    public void close() {
        if (owned && handle != 0) {
            Native.jsonFree(handle);
            handle = 0;
        }
    }

    public boolean isClosed() {
        return handle == 0;
    }

    private long requireOpen() {
        if (handle == 0) {
            throw new NclinkException(-13, "Json", "句柄已关闭");
        }
        return handle;
    }

    // ---------------------------------------------------------- 视图 -- //

    public JsonType type() {
        return JsonType.of(Native.jsonType(requireOpen()));
    }

    public boolean isNull() {
        return Native.jsonIsNull(requireOpen()) != 0;
    }

    public boolean isArray() {
        return type() == JsonType.ARRAY;
    }

    public boolean isObject() {
        return type() == JsonType.OBJECT;
    }

    /** 数组元素个数 / 对象成员个数（标量是 0）。 */
    public int size() {
        return Native.jsonCount(requireOpen());
    }

    /** 数组元素（借用视图）。 */
    public Json get(int index) {
        long found = Native.jsonArrayGet(requireOpen(), index);
        if (found == 0) {
            throw new IndexOutOfBoundsException("index " + index);
        }
        return view(found, this);
    }

    /** 对象成员（借用视图）；没有这个键返回 null。 */
    public Json get(String key) {
        long found = Native.jsonObjectGet(requireOpen(), key);
        return found == 0 ? null : view(found, this);
    }

    /** 对象成员（借用视图，按下标）。 */
    public Json valueAt(int index) {
        long found = Native.jsonObjectValueAt(requireOpen(), index);
        if (found == 0) {
            throw new IndexOutOfBoundsException("index " + index);
        }
        return view(found, this);
    }

    /** 对象第 index 个成员的键。 */
    public String keyAt(int index) {
        return Native.jsonObjectKeyAt(requireOpen(), index);
    }

    /** 对象成员键（不是对象时为空表）。 */
    public List<String> keys() {
        List<String> keys = new ArrayList<String>();
        if (isObject()) {
            for (int i = 0; i < size(); i++) {
                keys.add(keyAt(i));
            }
        }
        return keys;
    }

    /** 数组元素 / 对象成员值（借用视图）。 */
    public List<Json> values() {
        List<Json> values = new ArrayList<Json>();
        if (isArray()) {
            for (int i = 0; i < size(); i++) {
                values.add(get(i));
            }
        } else if (isObject()) {
            for (int i = 0; i < size(); i++) {
                values.add(valueAt(i));
            }
        }
        return values;
    }

    // ---------------------------------------------------------- 取值 -- //

    public long asLong(long defaultValue) {
        long[] out = new long[1];
        if (Native.jsonAsLong(requireOpen(), out) == 0) {
            return defaultValue;
        }
        return out[0];
    }

    public long asLong() {
        return asLong(0);
    }

    public double asDouble(double defaultValue) {
        double[] out = new double[1];
        if (Native.jsonAsDouble(requireOpen(), out) == 0) {
            return defaultValue;
        }
        return out[0];
    }

    public double asDouble() {
        return asDouble(0);
    }

    public boolean asBool(boolean defaultValue) {
        int[] out = new int[1];
        if (Native.jsonAsBool(requireOpen(), out) == 0) {
            return defaultValue;
        }
        return out[0] != 0;
    }

    public boolean asBool() {
        return asBool(false);
    }

    /** 只有字符串才给值，其它类型返回 null。 */
    public String asString() {
        return Native.jsonString(requireOpen());
    }

    /** 文本形式：字符串去引号、数字原样。 */
    public String asText() {
        return Native.jsonText(requireOpen());
    }

    /** 紧凑 JSON 文本（原样序列化，不丢精度）。 */
    public String encode() {
        return Native.jsonWrite(requireOpen());
    }

    /**
     * 转成 Java 原生对象：{@code Map<String,Object>} / {@code List<Object>} /
     * {@code String} / {@code Long} / {@code Double} / {@code Boolean} / null。
     */
    public Object toJavaObject() {
        long raw = requireOpen();
        switch (type()) {
            case NULL:
                return null;
            case BOOL:
                return Boolean.valueOf(asBool());
            case NUMBER: {
                String text = asText();
                if (text != null && (text.indexOf('.') >= 0 || text.indexOf('e') >= 0
                        || text.indexOf('E') >= 0)) {
                    return Double.valueOf(asDouble());
                }
                long[] out = new long[1];
                if (Native.jsonAsLong(raw, out) != 0) {
                    return Long.valueOf(out[0]);
                }
                return Double.valueOf(asDouble());
            }
            case STRING:
                return asString();
            case ARRAY: {
                List<Object> list = new ArrayList<Object>(size());
                for (int i = 0; i < size(); i++) {
                    list.add(get(i).toJavaObject());
                }
                return list;
            }
            default: {
                Map<String, Object> map = new LinkedHashMap<String, Object>();
                for (int i = 0; i < size(); i++) {
                    map.put(keyAt(i), valueAt(i).toJavaObject());
                }
                return map;
            }
        }
    }

    @Override
    public String toString() {
        return handle != 0 ? encode() : "<Json closed>";
    }
}
