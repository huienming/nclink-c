// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** ncl_node_type：模型里节点的种类。 */
public enum NodeType {
    BASE(0),
    ROOT(1),
    DEVICE(2),
    COMPONENT(3),
    DATA_ITEM(4),
    CONFIG(5);

    private final int code;

    NodeType(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static NodeType of(int code) {
        for (NodeType type : values()) {
            if (type.code == code) {
                return type;
            }
        }
        return BASE;
    }
}
