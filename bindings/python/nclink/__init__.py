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
from ._file import FileInfo
from ._json import Json, JsonType
from ._message import Event, Message, MessageType, Sample, SampleColumn, parse
from ._model import Model, Node, NodeType
from .client import DeviceClient
from .server import HttpEndpoint, Operation, Server

__all__ = [
    "NclinkError",
    "Json", "JsonType",
    "Model", "Node", "NodeType",
    "Sample", "SampleColumn", "Event", "Message", "MessageType", "parse",
    "DeviceClient",
    "Server", "Operation", "HttpEndpoint",
    "FileInfo",
    "LogLevel", "TlsOptions",
    "init", "shutdown", "is_open", "get_device", "version", "tls_available",
    "set_root", "root", "device_model",
    "log_init", "log_shutdown", "set_log_level", "set_console_log",
    "start_file_server", "stop_file_server",
    "file_need_compression", "file_total_chunks", "file_checksum",
    "file_attribute",
]


class TlsOptions:
    """`ssl://` / `tls://` 连接的 TLS 选项（给 `init(tls=...)`）。

    不设就是默认：校验证书链与主机名、用平台信任库。只有**带 TLS 编译的库 + 垫片**
    才起作用，先用 `nclink.tls_available()` 问一下。
    """

    __slots__ = ("ca_file", "client_cert", "client_key", "server_name",
                 "verify_peer")

    def __init__(self, ca_file=None, client_cert=None, client_key=None,
                 server_name=None, verify_peer=True):
        self.ca_file = ca_file          # PEM 证书束；None = 平台信任库
        self.client_cert = client_cert  # 双向 TLS 的客户端证书
        self.client_key = client_key
        self.server_name = server_name  # SNI / 主机名校验用；None = URL 里的主机名
        self.verify_peer = verify_peer  # 默认校验；自签调试才关


class LogLevel(IntEnum):
    """ncl_log_level。"""

    DEBUG = 0
    INFO = 1
    WARN = 2
    ERROR = 3
    FATAL = 4


def init(uri, username=None, password=None, tls=None):
    """建进程级连接（内部连 broker 并起客户端管理器）。一个进程调一次。

    `tls` 给一个 `TlsOptions`（或同名字段的 dict）就能连 `ssl://`——要带 TLS 编译的
    库与垫片，先用 `nclink.tls_available()` 问一下。
    """
    if not uri:
        raise ValueError("uri 不能为空，例如 tcp://127.0.0.1:1883")
    if isinstance(tls, dict):
        tls = TlsOptions(**tls)
    if tls is None:
        rc = lib.nclshim_open(encode(uri), encode(username), encode(password))
    else:
        rc = lib.nclshim_open_ex(encode(uri), encode(username), encode(password),
                                 encode(tls.ca_file), encode(tls.client_cert),
                                 encode(tls.client_key), encode(tls.server_name),
                                 1 if tls.verify_peer else 0)
    if rc != 0:
        raise NclinkError(rc, "init")


def tls_available():
    """当前的原生库有没有编 TLS（False 时 `ssl://` 会报 NOT_SUPPORTED）。"""
    return bool(lib.nclshim_tls_available())


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


def device_model():
    """五个语言设备端示例共用的设备模型（JSON 文本；编译在垫片里）。"""
    return decode(lib.nclshim_device_model())


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


# ------------------------------------------------------------ 文件通道 -- #

def start_file_server(port=0, root=None, username=None, password=None):
    """起进程级 FTP 端点（默认 127.0.0.1:2323，admin / 123456，根 = 安装根）。

    文件通道里**设备是 FTP 客户端**，本机得有个 FTP 服务端等着它来取/送；
    `init()` 时已经起过了，这里是给"先要文件后连 broker"的场合用的（幂等）。

    `port=0` / `root=None` / 账号留空就是默认值；要换端口、换根目录、换账号（比如
    2323 被占、或者对端不在这台机器上约定的端口）就传进来。
    """
    if port or root or username or password:
        rc = lib.nclshim_file_start_ftp_ex(int(port), encode(root), encode(username),
                                           encode(password))
    else:
        rc = lib.nclshim_file_start_ftp()
    if rc != 0:
        raise NclinkError(rc, "start_file_server")


def stop_file_server():
    """停掉进程级 FTP 端点。"""
    lib.nclshim_file_stop_ftp()


def file_need_compression(file_name):
    """这个扩展名的文件传输时要不要压缩（文本类为 True）。"""
    return bool(lib.nclshim_file_need_compression(encode(file_name)))


def file_total_chunks(size):
    """按 256 KB 一片算，这个字节数要几片。"""
    return int(lib.nclshim_file_total_chunks(int(size)))


def file_checksum(path):
    """本地文件内容的 SHA-256（小写十六进制）；读不了返回 None。"""
    text = lib.nclshim_file_checksum(encode(path))
    if not text:
        return None
    from ._ffi import take_text

    return take_text(text)


def file_attribute(path, parent=None):
    """本地文件/目录的属性（目录的 file_type 为 1）；拿不到返回 None。"""
    text = lib.nclshim_file_attribute_json(encode(path), encode(parent))
    if not text:
        return None
    from ._ffi import take_text

    return FileInfo.parse_list("[%s]" % take_text(text))[0]
