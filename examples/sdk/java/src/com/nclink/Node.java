// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.ArrayList;
import java.util.List;

/**
 * 模型里的一个节点（设备 / 组件 / 数据项 / 配置）。
 *
 * <p><b>借用</b>视图：它持有 {@link Model} 的引用，模型活着它就有效，
 * 不用（也不能）自己关。
 */
public final class Node {
    /* 子节点遍历用的类别号（垫片的约定，见 nclshim_node_count）。 */
    private static final int ITEMS = 0;
    private static final int DEVICES = 1;
    private static final int COMPONENTS = 2;
    private static final int CONFIGS = 3;

    private final long handle;
    private final Model model;

    Node(long handle, Model model) {
        this.handle = handle;
        this.model = model;
    }

    long handle() {
        return handle;
    }

    public NodeType type() {
        return NodeType.of(Native.nodeType(handle));
    }

    /** 模型文件里的类型名（如 NC_LINK_ROOT / AXIS）。 */
    public String typeName() {
        return Native.nodeTypeName(handle);
    }

    public String name() {
        return Native.nodeName(handle);
    }

    public String id() {
        return Native.nodeId(handle);
    }

    /** 数据项在模型里的路径（如 /AXIS@S/POWER@1）。 */
    public String path() {
        return Native.nodePath(handle);
    }

    public String description() {
        return Native.nodeDescription(handle);
    }

    /** 同类多路传感器的编号（没有就是 null）。 */
    public String number() {
        return Native.nodeNumber(handle);
    }

    public String dataType() {
        return Native.nodeDataType(handle);
    }

    public String valueType() {
        return Native.nodeValueType(handle);
    }

    public String mapping() {
        return Native.nodeMapping(handle);
    }

    public String source() {
        return Native.nodeSource(handle);
    }

    public String version() {
        return Native.nodeVersion(handle);
    }

    public String guid() {
        return Native.nodeGuid(handle);
    }

    public boolean settable() {
        return Native.nodeSettable(handle) != 0;
    }

    /** 数据项的初值（借用视图；没有就是 null）。 */
    public Json value() {
        long value = Native.nodeValue(handle);
        return value == 0 ? null : Json.view(value, this);
    }

    // ---------------------------------------------------------- 采样 -- //

    public boolean isSampleChannel() {
        return Native.nodeIsSampleChannel(handle) != 0;
    }

    public long sampleIntervalMs() {
        return Native.nodeSampleInterval(handle);
    }

    public long uploadIntervalMs() {
        return Native.nodeUploadInterval(handle);
    }

    /** 采样通道声明的各项路径。 */
    public List<String> sampleItemPaths() {
        int count = Native.nodeSampleItemCount(handle);
        List<String> paths = new ArrayList<String>(count);
        for (int i = 0; i < count; i++) {
            paths.add(Native.nodeSampleItemPath(handle, i));
        }
        return paths;
    }

    // -------------------------------------------------------- 子节点 -- //

    public int childCount(int kind) {
        return Native.nodeCount(handle, kind);
    }

    public Node childAt(int kind, int index) {
        long child = Native.nodeAt(handle, kind, index);
        return child == 0 ? null : new Node(child, model);
    }

    public List<Node> children(int kind) {
        int count = childCount(kind);
        List<Node> children = new ArrayList<Node>(count);
        for (int i = 0; i < count; i++) {
            children.add(childAt(kind, i));
        }
        return children;
    }

    public List<Node> devices() {
        return children(DEVICES);
    }

    public List<Node> components() {
        return children(COMPONENTS);
    }

    public List<Node> configs() {
        return children(CONFIGS);
    }

    public List<Node> dataItems() {
        return children(ITEMS);
    }

    @Override
    public String toString() {
        return "<Node " + type() + " " + id() + " " + name() + ">";
    }
}
