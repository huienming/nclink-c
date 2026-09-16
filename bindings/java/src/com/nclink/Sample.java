// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.ArrayList;
import java.util.List;

/**
 * 一条采样报文。
 *
 * <p>回调里拿到的已经是**快照**（值是 Java 原生对象），出了回调照样能用；
 * 原始报文文本在 {@link #rawJson()}。语义与 C 的
 * {@code ncl_message_sample_value_at()} 完全一致。
 */
public final class Sample {
    private final String topic;
    private final String id;
    private final String beginTime;
    private final long intervalMs;
    private final long uploadIntervalMs;
    private final boolean complete;
    private final String rawJson;
    private final List<SampleColumn> columns;
    private final int rows;

    Sample(String topic, String id, String beginTime, long intervalMs,
           long uploadIntervalMs, boolean complete, String rawJson,
           List<SampleColumn> columns, int rows) {
        this.topic = topic;
        this.id = id;
        this.beginTime = beginTime;
        this.intervalMs = intervalMs;
        this.uploadIntervalMs = uploadIntervalMs;
        this.complete = complete;
        this.rawJson = rawJson;
        this.columns = columns;
        this.rows = rows;
    }

    /** 回调里把原生报文拷成快照（msg 是回调给的借用句柄，只在回调期间有效）。 */
    static Sample capture(String topic, long msg) {
        int count = Native.sampleColumns(msg);
        List<SampleColumn> columns = new ArrayList<SampleColumn>(count);
        for (int col = 0; col < count; col++) {
            int points = Native.sampleColumnPoints(msg, col);
            Object[] values = new Object[points];
            for (int i = 0; i < points; i++) {
                values[i] = read(Native.sampleColumnValueAt(msg, col, i));
            }
            columns.add(new SampleColumn(Native.samplePath(msg, col),
                    Native.sampleColumnSlots(msg, col),
                    Native.sampleColumnNested(msg, col) != 0,
                    Native.sampleColumnEncoding(msg, col), values));
        }
        return new Sample(topic, Native.sampleId(msg), Native.sampleBeginTime(msg),
                Native.sampleInterval(msg), Native.sampleUploadInterval(msg),
                Native.sampleIsComplete(msg) != 0, Native.messageWrite(msg), columns,
                Native.sampleRows(msg));
    }

    /** 借用的 JSON 句柄 -> Java 原生值（立刻拷出来，不保留句柄）。 */
    private static Object read(long json) {
        return json == 0 ? null : Json.borrowed(json).toJavaObject();
    }

    public String topic() {
        return topic;
    }

    /** 通道 id（如 sample_channel0）。 */
    public String id() {
        return id;
    }

    /** 上报窗口起点（epoch 毫秒字符串）。 */
    public String beginTime() {
        return beginTime;
    }

    /** 采样周期（毫秒）。 */
    public long intervalMs() {
        return intervalMs;
    }

    public long uploadIntervalMs() {
        return uploadIntervalMs;
    }

    /** 外层（表头与槽位）是否对齐。 */
    public boolean isComplete() {
        return complete;
    }

    /** 原始报文 JSON 文本。 */
    public String rawJson() {
        return rawJson;
    }

    public List<SampleColumn> columns() {
        return columns;
    }

    /** 行数 = 数据最多的那一列的点数。 */
    public int rows() {
        return rows;
    }

    /** 按行取值：采样率低的列会连着几行返回同一个点（覆盖该行的第一个点）。 */
    public Object valueAt(int row, int column) {
        if (column < 0 || column >= columns.size()) {
            throw new IndexOutOfBoundsException("column " + column);
        }
        if (row < 0 || row >= rows) {
            throw new IndexOutOfBoundsException("row " + row);
        }
        Object[] values = columns.get(column).values;
        if (values.length == 0) {
            return null;
        }
        return values[row * values.length / rows];
    }

    public double getDouble(int row, int column, double defaultValue) {
        Object value = valueAt(row, column);
        if (value instanceof Number) {
            return ((Number) value).doubleValue();
        }
        if (value instanceof String) {
            try {
                return Double.parseDouble((String) value);
            } catch (NumberFormatException ignored) {
                return defaultValue;
            }
        }
        return defaultValue;
    }

    public double getDouble(int row, int column) {
        return getDouble(row, column, 0);
    }

    public long getLong(int row, int column, long defaultValue) {
        Object value = valueAt(row, column);
        if (value instanceof Number) {
            return ((Number) value).longValue();
        }
        if (value instanceof String) {
            try {
                return (long) Double.parseDouble((String) value);
            } catch (NumberFormatException ignored) {
                return defaultValue;
            }
        }
        return defaultValue;
    }

    public long getLong(int row, int column) {
        return getLong(row, column, 0);
    }

    /** 按行取字符串（不是字符串时用文本形式）。 */
    public String getString(int row, int column) {
        Object value = valueAt(row, column);
        return value == null ? null : String.valueOf(value);
    }

    /** 表头一行。 */
    public String header(String separator) {
        StringBuilder text = new StringBuilder();
        for (SampleColumn column : columns) {
            if (text.length() > 0) {
                text.append(separator);
            }
            text.append(column.path == null ? "" : column.path);
        }
        return text.toString();
    }

    @Override
    public String toString() {
        return "Sample " + topic + " id=" + id + " rows=" + rows + " columns="
                + columns.size();
    }
}
