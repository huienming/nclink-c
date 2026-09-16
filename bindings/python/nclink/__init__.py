# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""NC-Link C 实现（GB/T 41970-2022 / 协议 3.0.0）的 Python 绑定。

用法（设备客户端视角）::

    import nclink

    nclink.init("tcp://127.0.0.1:1883")          # 进程级连接，一次
    nclink.log_init()                            # 日志走 <root>/log/out.txt

    with nclink.get_device("V2023A7B762") as device:
        with device.probe() as model:            # 拉模型
            print(model.root.name, [n.path for n in model.root.children(0)])
        with device.get_value("/STATUS") as value:
            print("STATUS =", value.to_python())
        device.set_value("/STATUS", 42)           # 写值
        with device.method_call("/plc/getCount", check=True) as reply:
            print(reply.to_python())

        device.subscribe_samples(2, lambda topic, sample: print(sample.rows))
        device.subscribe_events(2, lambda topic, event: print(event.key))
        time.sleep(5)

    nclink.shutdown()

原生层是三种托管绑定共用的 `bindings/native/nclink_shim.c`（扁平 C ABI：不透明
句柄 + 标量 + UTF-8 文本），Python 侧用 ctypes 调它，不依赖 C 结构体布局。

所有权：

* **自有**（用完 close / with）：`Json`、`Model`、`DeviceClient`；
* **借用**（不用管，宿主活着就有效）：`Node`、`Json` 的下标与成员视图；
* 采样/事件回调拿到的是**快照**，出了回调照样能用。
"""

from __future__ import annotations

from enum import IntEnum

from ._ffi import NclinkError, decode, encode, lib, version
from ._json import Json, JsonType
from ._message import Event, Message, MessageType, Sample, SampleColumn, parse
from ._model import Model, Node, NodeType
from .client import DeviceClient

__all__ = [
    "NclinkError",
    "Json", "JsonType",
    "Model", "Node", "NodeType",
    "Sample", "SampleColumn", "Event", "Message", "MessageType", "parse",
    "DeviceClient",
    "LogLevel",
    "init", "shutdown", "is_open", "get_device", "version",
    "set_root", "root",
    "log_init", "log_shutdown", "set_log_level", "set_console_log",
]


class LogLevel(IntEnum):
    """ncl_log_level。"""

    DEBUG = 0
    INFO = 1
    WARN = 2
    ERROR = 3
    FATAL = 4


def init(uri, username=None, password=None):
    """建进程级连接（内部连 broker 并起客户端管理器）。一个进程调一次。"""
    if not uri:
        raise ValueError("uri 不能为空，例如 tcp://127.0.0.1:1883")
    rc = lib.nclshim_open(encode(uri), encode(username), encode(password))
    if rc != 0:
        raise NclinkError(rc, "init")


def shutdown():
    """断开连接、释放所有客户端（没连过就是空操作）。"""
    if is_open():
        lib.nclshim_close()


def is_open():
    """进程级连接是否已经建好。"""
    return bool(lib.nclshim_is_open())


def get_device(sn):
    """取（必要时创建）某个 SN 的设备客户端；同一个 SN 只有一份。"""
    if not sn:
        raise ValueError("sn 不能为空")
    handle = lib.nclshim_client_get(encode(sn))
    if not handle:
        raise NclinkError(-6, "get_device", "取设备客户端失败: %s（先调 init？）" % sn)
    return DeviceClient(sn, handle)


def set_root(path):
    """安装根目录（conf/、bin/、log/ 都在它下面）。"""
    lib.nclshim_env_set_root(encode(path))


def root():
    """当前的安装根目录。"""
    return decode(lib.nclshim_env_root())


def log_init(directory=None):
    """初始化日志；`directory` 为 None 时用 `<root>/log`。"""
    return bool(lib.nclshim_log_init(encode(directory)))


def log_shutdown():
    """关闭日志（写盘线程回收）。"""
    lib.nclshim_log_shutdown()


def set_log_level(level):
    """日志级别（LogLevel 或整数）。"""
    lib.nclshim_log_set_level(int(level))


def set_console_log(enabled):
    """是否同时往控制台打（默认开）。"""
    lib.nclshim_log_set_console(1 if enabled else 0)
