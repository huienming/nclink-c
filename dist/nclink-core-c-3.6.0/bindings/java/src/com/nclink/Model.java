// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 设备数据模型（整棵树）。
 *
 * <p><b>自有</b>对象：{@code probe()} / {@code parse()} 的返回值，用完
 * {@link #close()}（或 try-with-resources）。
 */
public final class Model implements AutoCloseable {
    private long handle;
    private final boolean owned;
    private final Object owner;      // 借用视图：钉住宿主（服务器）

    public Model(long handle) {
        this(handle, true, null);
    }

    private Model(long handle, boolean owned, Object owner) {
        if (handle == 0) {
            throw new NclinkException(-2, "Model", "空句柄");
        }
        this.handle = handle;
        this.owned = owned;
        this.owner = owner;
    }

    /**
     * 借用视图：句柄归别人（设备端自己的模型），{@link #close()} 只是不再使用。
     */
    static Model borrowed(long handle, Object owner) {
        return new Model(handle, false, owner);
    }

    /** 解析模型文档；{@code null} 或空串 = 库内置的默认模型。 */
    public static Model parse(String json) {
        String text = (json == null || json.isEmpty()) ? null : json;
        long root = Native.modelParse(text);
        if (root == 0) {
            throw new NclinkException(-4, "Model.parse", "模型文档不合法");
        }
        return new Model(root);
    }

    /** 库内置的默认模型。 */
    public static Model parse() {
        return parse(null);
    }

    /** 根节点（借用视图）。 */
    public Node root() {
        return new Node(requireOpen(), this);
    }

    /** 按节点 id 找节点，找不到返回 null。 */
    public Node findById(String id) {
        long node = Native.modelFindById(requireOpen(), id);
        return node == 0 ? null : new Node(node, this);
    }

    /** 整棵树的 JSON 文本。 */
    public String toJson() {
        return Native.modelWrite(requireOpen());
    }

    @Override
    public void close() {
        if (handle != 0 && owned) {
            Native.modelFree(handle);
        }
        if (handle != 0) {
            handle = 0;
        }
    }

    public boolean isClosed() {
        return handle == 0;
    }

    private long requireOpen() {
        if (handle == 0) {
            throw new NclinkException(-13, "Model", "句柄已关闭");
        }
        return handle;
    }

    @Override
    public String toString() {
        return handle != 0 ? toJson() : "<Model closed>";
    }
}
