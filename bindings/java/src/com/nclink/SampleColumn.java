// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** 采样报文里的一列（= 一个采样项）。 */
public final class SampleColumn {
    final String path;
    final int slots;
    final boolean nested;
    final String encoding;
    final Object[] values;

    SampleColumn(String path, int slots, boolean nested, String encoding, Object[] values) {
        this.path = path;
        this.slots = slots;
        this.nested = nested;
        this.encoding = encoding;
        this.values = values;
    }

    /** 数据项路径。设备段（/MACHINE）不写在这里，要拼回模型的绝对路径就加上它。 */
    public String path() {
        return path;
    }

    /** 槽位数（通道 sampleInterval 的个数）。 */
    public int slots() {
        return slots;
    }

    /** 该列总点数。 */
    public int points() {
        return values.length;
    }

    /** 每槽是多点（批量）还是一点。 */
    public boolean isNested() {
        return nested;
    }

    /** 原始编码方式（没有就是 null）。 */
    public String encoding() {
        return encoding;
    }

    /** 该列按顺序拉平的第 index 个点。 */
    public Object valueAt(int index) {
        return values[index];
    }

    @Override
    public String toString() {
        return String.format("%s: %d 个槽位 × 每槽约 %d 点 = %d 点%s", path, slots,
                slots > 0 ? values.length / slots : 0, values.length,
                nested ? "（批量）" : "");
    }
}
