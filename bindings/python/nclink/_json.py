# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""ncl_json 的 Python 封装。

Python 侧不做 JSON 解析：文本进 `Json.parse()`，值出来是库自己的解析器给的句柄；
要 Python 原生对象就 `to_python()`。

所有权与 C# 绑定一致：

* **自有**句柄（`parse()` / `clone()` / `client.get_value()` 的返回值）用完要
  `close()`（或者用 `with`），忘了也会由 GC 兜底；
* **借用**视图（`json["key"]` / `json[0]` / `items()`）不用关，它们持有宿主引用，
  宿主活着它们就有效。
"""

from __future__ import annotations

import ctypes
import weakref
from enum import IntEnum

from ._ffi import NclinkError, decode, lib, take_text

__all__ = ["Json", "JsonType"]


class JsonType(IntEnum):
    """ncl_json_type：与 C 的 ncl_json_type 一一对应。"""

    NULL = 0
    BOOL = 1
    NUMBER = 2
    STRING = 3
    ARRAY = 4
    OBJECT = 5


class Json:
    """一个 JSON 值（对象、数组、标量都算）。"""

    __slots__ = ("_handle", "_owner", "_owned", "_finalizer", "__weakref__")

    def __init__(self, handle, owner=None, owned=False):
        self._handle = handle
        self._owner = owner          # 借用视图：钉住宿主，句柄才有效
        self._owned = bool(owned) and handle is not None
        # 忘了 close() 时由它兜底；close() 之后要 detach，否则就是二次释放
        self._finalizer = None
        if self._owned:
            self._finalizer = weakref.finalize(self, lib.nclshim_json_free, handle)

    # ------------------------------------------------------------ 构造 -- #

    @classmethod
    def parse(cls, text):
        """解析 JSON 文本；不合法抛 NclinkError。"""
        if isinstance(text, Json):
            return text.clone()
        handle = lib.nclshim_json_parse(None if text is None else str(text).encode("utf-8"))
        if not handle:
            raise NclinkError(-3, "Json.parse", "JSON 文本不合法")
        return cls(handle, owned=True)

    @classmethod
    def null(cls):
        return cls.parse("null")

    def clone(self):
        handle = lib.nclshim_json_clone(self._handle)
        if not handle:
            raise NclinkError(-2, "Json.clone", "内存不足")
        return Json(handle, owned=True)

    # ------------------------------------------------------ 所有权 -- #

    def close(self):
        """释放自有句柄（借用视图是空操作，可重复调用）。"""
        if self._owned and self._handle:
            lib.nclshim_json_free(self._handle)
            if self._finalizer is not None:
                self._finalizer.detach()
                self._finalizer = None
            self._owned = False
            self._handle = None
            self._owner = None

    @property
    def closed(self):
        return self._handle is None

    def _check_open(self):
        if self._handle is None:
            raise NclinkError(-2, "Json", "句柄已关闭")
        return self._handle

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    # ---------------------------------------------------------- 视图 -- #

    @property
    def type(self):
        """JsonType。"""
        return JsonType(lib.nclshim_json_type(self._check_open()))

    @property
    def is_null(self):
        return bool(lib.nclshim_json_is_null(self._check_open()))

    @property
    def is_array(self):
        return self.type == JsonType.ARRAY

    @property
    def is_object(self):
        return self.type == JsonType.OBJECT

    @property
    def count(self):
        """数组元素个数 / 对象成员个数（标量是 0）。"""
        return int(lib.nclshim_json_count(self._check_open()))

    def __len__(self):
        return self.count

    def __getitem__(self, key):
        if isinstance(key, str):
            found = self.get(key)
            if found is None:
                raise KeyError(key)
            return found
        return self.value_at(int(key))

    def get(self, key):
        """对象的成员，取不到返回 None（借用视图）。"""
        handle = lib.nclshim_json_object_get(self._check_open(), str(key).encode("utf-8"))
        return None if not handle else Json(handle, owner=self)

    def value_at(self, index):
        """数组元素 / 对象第 index 个成员的值（借用视图）。"""
        handle = lib.nclshim_json_array_get(self._check_open(), int(index))
        if not handle:
            handle = lib.nclshim_json_object_value_at(self._check_open(), int(index))
        if not handle:
            raise IndexError(index)
        return Json(handle, owner=self)

    def key_at(self, index):
        """对象第 index 个成员的键。"""
        return decode(lib.nclshim_json_object_key_at(self._check_open(), int(index)))

    def keys(self):
        if not self.is_object:
            return []
        return [self.key_at(i) for i in range(self.count)]

    def values(self):
        if self.is_object:
            return [self.value_at(i) for i in range(self.count)]
        if self.is_array:
            return [self.value_at(i) for i in range(self.count)]
        return []

    def items(self):
        """对象成员 (键, 值) 迭代；不是对象时为空。"""
        if not self.is_object:
            return iter(())
        return ((self.key_at(i), self.value_at(i)) for i in range(self.count))

    # ---------------------------------------------------------- 取值 -- #

    def as_long(self, default=0):
        out = ctypes.c_longlong()
        if not lib.nclshim_json_as_int(self._check_open(), ctypes.byref(out)):
            return default
        return int(out.value)

    as_int = as_long

    def as_double(self, default=0.0):
        out = ctypes.c_double()
        if not lib.nclshim_json_as_double(self._check_open(), ctypes.byref(out)):
            return default
        return float(out.value)

    def as_bool(self, default=False):
        out = ctypes.c_int()
        if not lib.nclshim_json_as_bool(self._check_open(), ctypes.byref(out)):
            return default
        return bool(out.value)

    def as_string(self):
        """只有字符串才给值，其它类型返回 None。"""
        return decode(lib.nclshim_json_string(self._check_open()))

    def as_text(self):
        """文本形式：字符串去引号、数字原样（都是 str）。"""
        return take_text(lib.nclshim_json_text(self._check_open()))

    def encode(self):
        """紧凑 JSON 文本（原样序列化，不丢精度）。"""
        return take_text(lib.nclshim_json_write(self._check_open()))

    def to_python(self):
        """转成 Python 原生对象：dict / list / str / int / float / bool / None。"""
        kind = self.type
        if kind == JsonType.NULL:
            return None
        if kind == JsonType.BOOL:
            return self.as_bool()
        if kind == JsonType.NUMBER:
            text = self.as_text() or "0"
            if any(c in text for c in ".eE"):
                return self.as_double()
            return int(text)
        if kind == JsonType.STRING:
            return self.as_string()
        if kind == JsonType.ARRAY:
            return [self.value_at(i).to_python() for i in range(self.count)]
        return {self.key_at(i): self.value_at(i).to_python() for i in range(self.count)}

    def __str__(self):
        return self.encode() if self._handle else "<Json closed>"

    __repr__ = __str__
