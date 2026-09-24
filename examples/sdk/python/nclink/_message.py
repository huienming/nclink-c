# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""报文：采样、事件，以及解析。

Python 侧拿到的都是**快照**（纯 Python 对象），不是原生句柄：

* 回调里那条报文只在回调期间有效，必须在回调里拷出来（`Sample` / `Event` 就是
  干这个的，见 `Sample.capture()`）；
* `parse()` 解析出来的报文解析完就把原生报文放掉，返回的同样是快照。

所以采样/事件对象出了回调、出了 `with` 都还能用，也不存在"忘了关"的问题。
"""

from __future__ import annotations

import ctypes
from enum import IntEnum

from ._ffi import NclinkError, decode, encode, lib, take_text
from ._json import Json

__all__ = ["Message", "MessageType", "Sample", "SampleColumn", "Event", "parse"]


class MessageType(IntEnum):
    """ncl_msg_type。"""

    UNKNOWN = 0
    PING = 1
    PONG = 2
    PROBE_VERSION = 3
    REGISTER_REQUEST = 4
    REGISTER_RESPONSE = 5
    QUERY_REQUEST = 6
    QUERY_RESPONSE = 7
    SET_REQUEST = 8
    SET_RESPONSE = 9
    PROBE_QUERY_REQUEST = 10
    PROBE_QUERY_RESPONSE = 11
    PROBE_SET_REQUEST = 12
    PROBE_SET_RESPONSE = 13
    SAMPLE = 14
    EVENT = 15
    METHOD_CALL_REQUEST = 16
    METHOD_CALL_RESPONSE = 17


def _json_value(handle):
    """借用的 JSON 句柄 -> Python 原生值（立刻拷出来，不保留句柄）。"""
    if not handle:
        return None
    return Json(handle).to_python()


class SampleColumn:
    """采样报文里的一列（= 一个采样项）。"""

    __slots__ = ("path", "slots", "is_nested", "encoding", "values")

    def __init__(self, path, slots, is_nested, encoding, values):
        self.path = path                # 数据项路径（设备内路径：不带 /MACHINE 段）
        self.slots = slots              # 槽位数（通道 sampleInterval 的个数）
        self.is_nested = is_nested      # 每槽是多点（批量）还是一点
        self.encoding = encoding        # 原始编码方式（没有就是 None）
        self.values = values            # 该列按顺序拉平的点

    @property
    def points(self):
        """该列总点数。"""
        return len(self.values)

    def value_at(self, index):
        if index < 0 or index >= len(self.values):
            raise IndexError(index)
        return self.values[index]

    def __len__(self):
        return len(self.values)

    def __str__(self):
        return "%s: %d 个槽位 × %s = %d 点%s" % (
            self.path, self.slots, "多点" if self.is_nested else "约 1 点",
            self.points, "（批量）" if self.is_nested else "")

    __repr__ = __str__


class Sample:
    """一条采样报文（设备发什么就是什么，值是 Python 原生对象）。"""

    __slots__ = ("topic", "id", "begin_time", "interval_ms", "upload_interval_ms",
                 "is_complete", "raw_json", "columns", "rows")

    def __init__(self, topic, id, begin_time, interval_ms, upload_interval_ms,
                 is_complete, raw_json, columns, rows):
        self.topic = topic
        self.id = id                        # 通道 id（如 sample_channel0）
        self.begin_time = begin_time        # 上报窗口起点（epoch 毫秒字符串）
        self.interval_ms = interval_ms      # 采样周期（毫秒）
        self.upload_interval_ms = upload_interval_ms
        self.is_complete = is_complete      # 外层（表头与槽位）是否对齐
        self.raw_json = raw_json            # 原始报文 JSON 文本
        self.columns = columns
        self.rows = rows                    # 行数 = 数据最多的那一列的点数

    @classmethod
    def capture(cls, msg, topic=None):
        """回调里把原生报文拷成快照（msg 是回调给的借用句柄）。"""
        columns = []
        for col in range(int(lib.nclshim_sample_columns(msg))):
            points = int(lib.nclshim_sample_column_points(msg, col))
            values = []
            for i in range(points):
                values.append(_json_value(lib.nclshim_sample_column_value_at(msg, col, i)))
            columns.append(SampleColumn(
                path=decode(lib.nclshim_sample_path(msg, col)),
                slots=int(lib.nclshim_sample_column_slots(msg, col)),
                is_nested=bool(lib.nclshim_sample_column_nested(msg, col)),
                encoding=decode(lib.nclshim_sample_column_encoding(msg, col)),
                values=values))
        return cls(topic=topic,
                   id=decode(lib.nclshim_sample_id(msg)),
                   begin_time=decode(lib.nclshim_sample_begin_time(msg)),
                   interval_ms=int(lib.nclshim_sample_interval(msg)),
                   upload_interval_ms=int(lib.nclshim_sample_upload_interval(msg)),
                   is_complete=bool(lib.nclshim_sample_is_complete(msg)),
                   raw_json=take_text(lib.nclshim_message_write(msg)),
                   columns=columns,
                   rows=int(lib.nclshim_sample_rows(msg)))

    def value_at(self, row, column):
        """按行取值：采样率低的列会连着几行返回同一个点（覆盖该行的第一个点）。"""
        if column < 0 or column >= len(self.columns):
            raise IndexError(column)
        if row < 0 or row >= self.rows:
            raise IndexError(row)
        values = self.columns[column].values
        if not values:
            return None
        return values[row * len(values) // self.rows]

    def get_double(self, row, column, default=0.0):
        value = self.value_at(row, column)
        if isinstance(value, bool):
            return float(value)
        if isinstance(value, (int, float)):
            return float(value)
        if isinstance(value, str):
            try:
                return float(value)
            except ValueError:
                return default
        return default

    def get_long(self, row, column, default=0):
        value = self.value_at(row, column)
        if isinstance(value, bool):
            return int(value)
        if isinstance(value, int):
            return value
        if isinstance(value, float):
            return int(value)
        if isinstance(value, str):
            try:
                return int(float(value))
            except ValueError:
                return default
        return default

    def get_string(self, row, column):
        value = self.value_at(row, column)
        if value is None:
            return None
        if isinstance(value, float):
            return repr(value)
        if isinstance(value, bool):
            return "true" if value else "false"
        return str(value)

    def header(self, separator=" "):
        """表头一行（列路径用 separator 拼起来）。"""
        return separator.join(column.path or "" for column in self.columns)

    def __str__(self):
        return "Sample %s id=%s rows=%d columns=%d" % (
            self.topic, self.id, self.rows, len(self.columns))

    __repr__ = __str__


class Event:
    """一条事件报文。"""

    __slots__ = ("topic", "id", "time", "key", "value", "raw_json")

    def __init__(self, topic, id, time, key, value, raw_json):
        self.topic = topic
        self.id = id
        self.time = time
        self.key = key
        self.value = value
        self.raw_json = raw_json

    @classmethod
    def capture(cls, msg, topic=None):
        return cls(topic=topic,
                   id=decode(lib.nclshim_event_id(msg)),
                   time=decode(lib.nclshim_event_time(msg)),
                   key=decode(lib.nclshim_event_key(msg)),
                   value=_json_value(lib.nclshim_event_value(msg)),
                   raw_json=take_text(lib.nclshim_message_write(msg)))

    def __str__(self):
        return "Event %s id=%s key=%s value=%s" % (
            self.topic, self.id, self.key, self.value)

    __repr__ = __str__


class Message:
    """既不是采样也不是事件的报文：留类型与 JSON 文本。"""

    __slots__ = ("topic", "type", "raw_json")

    def __init__(self, topic, type, raw_json):
        self.topic = topic
        self.type = type
        self.raw_json = raw_json

    def to_python(self):
        """报文 JSON -> Python 原生对象。"""
        with Json.parse(self.raw_json) as parsed:
            return parsed.to_python()

    def __str__(self):
        return "Message %s type=%s" % (self.topic, self.type.name)

    __repr__ = __str__


def parse(topic, payload):
    """把一条 MQTT 报文按库的规则解码：采样 -> Sample，事件 -> Event，其它 -> Message。

    `payload` 收 bytes / bytearray / str（不是 NUL 结尾也没关系）。
    """
    if topic is None or payload is None:
        raise ValueError("topic / payload 都不能为空")
    data = payload if isinstance(payload, (bytes, bytearray)) else str(payload).encode("utf-8")
    data = bytes(data)
    buffer = ctypes.create_string_buffer(data, max(len(data), 1))
    handle = lib.nclshim_message_parse(encode(topic),
                                       ctypes.cast(buffer, ctypes.c_void_p),
                                       len(data))
    if not handle:
        raise NclinkError(-3, "Message.parse", "报文解析失败: %s" % topic)
    try:
        kind = int(lib.nclshim_message_type(handle))
        if kind == MessageType.SAMPLE:
            return Sample.capture(handle, topic)
        if kind == MessageType.EVENT:
            return Event.capture(handle, topic)
        try:
            named = MessageType(kind)
        except ValueError:
            named = MessageType.UNKNOWN
        return Message(topic, named, take_text(lib.nclshim_message_write(handle)))
    finally:
        lib.nclshim_message_free(handle)
