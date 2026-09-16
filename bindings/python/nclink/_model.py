# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""设备数据模型（ncl_node 树）的封装。

* `Model` 是**自有**对象（`probe()` / `parse()` 的返回值），用完 `close()`；
* `Node` 是**借用**视图：它持有 `Model` 的引用，模型活着它就有效，不用关。
"""

from __future__ import annotations

import weakref
from enum import IntEnum

from ._ffi import NclinkError, decode, lib, take_text
from ._json import Json

__all__ = ["Model", "Node", "NodeType"]

# 子节点遍历用的类别号（垫片的约定：见 nclshim_node_count）
_DEVICES = 1
_COMPONENTS = 2
_CONFIGS = 3
_DATA_ITEMS = 0


class NodeType(IntEnum):
    """ncl_node_type：模型里节点的种类。"""

    BASE = 0
    ROOT = 1
    DEVICE = 2
    COMPONENT = 3
    DATA_ITEM = 4
    CONFIG = 5


class Model:
    """整棵模型树。"""

    __slots__ = ("_handle", "_finalizer", "__weakref__")

    def __init__(self, handle):
        if not handle:
            raise NclinkError(-2, "Model", "空句柄")
        self._handle = handle
        # 忘了 close() 时由它兜底；close() 之后要 detach，否则就是二次释放
        self._finalizer = weakref.finalize(self, lib.nclshim_model_free, handle)

    @classmethod
    def parse(cls, json_text=None):
        """解析模型文档；None / 空串 = 库内置的默认模型。"""
        raw = None
        if json_text is not None:
            raw = str(json_text).encode("utf-8")
            if not raw:
                raw = None
        handle = lib.nclshim_model_parse(raw)
        if not handle:
            raise NclinkError(-4, "Model.parse", "模型文档不合法")
        return cls(handle)

    @property
    def root(self):
        """根节点（借用视图）。"""
        return Node(self._handle, owner=self)

    def find_by_id(self, node_id):
        """按节点 id 找节点，找不到返回 None。"""
        handle = lib.nclshim_model_find_by_id(self._handle, str(node_id).encode("utf-8"))
        return None if not handle else Node(handle, owner=self)

    def to_json(self):
        """整棵树的 JSON 文本。"""
        self._check_open()
        return take_text(lib.nclshim_model_write(self._handle))

    def close(self):
        if self._handle:
            lib.nclshim_model_free(self._handle)
            if self._finalizer is not None:
                self._finalizer.detach()
                self._finalizer = None
            self._handle = None

    @property
    def closed(self):
        return self._handle is None

    def _check_open(self):
        if self._handle is None:
            raise NclinkError(-2, "Model", "句柄已关闭")
        return self._handle

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    def __str__(self):
        return self.to_json() if self._handle else "<Model closed>"


class Node:
    """模型里的一个节点（设备 / 组件 / 数据项 / 配置）。借用视图，不用关。"""

    __slots__ = ("_handle", "_owner")

    def __init__(self, handle, owner):
        self._handle = handle
        self._owner = owner          # 钉住 Model，句柄才有效

    # ------------------------------------------------------------ 字段 -- #

    @property
    def type(self):
        """NodeType。"""
        return NodeType(lib.nclshim_node_type(self._handle))

    @property
    def type_name(self):
        """模型文件里的类型名（如 NC_LINK_ROOT / AXIS）。"""
        return decode(lib.nclshim_node_type_name(self._handle))

    @property
    def name(self):
        return decode(lib.nclshim_node_name(self._handle))

    @property
    def id(self):
        return decode(lib.nclshim_node_id(self._handle))

    @property
    def path(self):
        """数据项在模型里的路径（如 /AXIS@S/POWER@1）。"""
        return decode(lib.nclshim_node_path(self._handle))

    @property
    def description(self):
        return decode(lib.nclshim_node_description(self._handle))

    @property
    def number(self):
        """同类多路传感器的编号（没有就是 None）。"""
        return decode(lib.nclshim_node_number(self._handle))

    @property
    def data_type(self):
        return decode(lib.nclshim_node_data_type(self._handle))

    @property
    def value_type(self):
        return decode(lib.nclshim_node_value_type(self._handle))

    @property
    def mapping(self):
        return decode(lib.nclshim_node_mapping(self._handle))

    @property
    def source(self):
        return decode(lib.nclshim_node_source(self._handle))

    @property
    def version(self):
        return decode(lib.nclshim_node_version(self._handle))

    @property
    def guid(self):
        return decode(lib.nclshim_node_guid(self._handle))

    @property
    def settable(self):
        return bool(lib.nclshim_node_settable(self._handle))

    @property
    def value(self):
        """数据项的初值（借用视图；没有就是 None）。"""
        handle = lib.nclshim_node_value(self._handle)
        return None if not handle else Json(handle, owner=self)

    # ---------------------------------------------------------- 采样 -- #

    @property
    def is_sample_channel(self):
        return bool(lib.nclshim_node_is_sample_channel(self._handle))

    @property
    def sample_interval_ms(self):
        return int(lib.nclshim_node_sample_interval(self._handle))

    @property
    def upload_interval_ms(self):
        return int(lib.nclshim_node_upload_interval(self._handle))

    def sample_item_paths(self):
        """采样通道声明的各项路径。"""
        count = int(lib.nclshim_node_sample_item_count(self._handle))
        paths = []
        for i in range(count):
            raw = lib.nclshim_node_sample_item_path(self._handle, i)
            paths.append(take_text(raw))
        return paths

    # -------------------------------------------------------- 子节点 -- #

    def child_count(self, kind):
        return int(lib.nclshim_node_count(self._handle, int(kind)))

    def child_at(self, kind, index):
        handle = lib.nclshim_node_at(self._handle, int(kind), int(index))
        return None if not handle else Node(handle, owner=self._owner)

    def children(self, kind):
        return [self.child_at(kind, i) for i in range(self.child_count(kind))]

    @property
    def devices(self):
        return self.children(_DEVICES)

    @property
    def components(self):
        return self.children(_COMPONENTS)

    @property
    def configs(self):
        return self.children(_CONFIGS)

    @property
    def data_items(self):
        return self.children(_DATA_ITEMS)

    def __str__(self):
        return "<Node %s %s %s>" % (self.type.name, self.id, self.name or "")

    __repr__ = __str__
