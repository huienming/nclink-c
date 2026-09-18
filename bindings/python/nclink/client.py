# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""设备客户端（ncl_client）：按 SN 读值/写值/探测/采样订阅/事件订阅。"""

from __future__ import annotations

import ctypes
import json as _json_stdlib

from ._ffi import NclinkError, MESSAGE_CALLBACK, decode, encode, lib, take_text
from . import _file
from ._json import Json
from ._message import Event, Sample
from ._model import Model

__all__ = ["DeviceClient"]


def _as_json_text(value):
    """要把一个值发出去时怎么变成 JSON 文本。

    * `Json` -> 它自己的紧凑文本；
    * `str` / `bytes` -> **就是** JSON 文本（要发字符串记得带引号）；
    * 其它 Python 对象（数字、bool、None、list、dict）-> json.dumps。
    """
    if isinstance(value, Json):
        return value.encode()
    if isinstance(value, (str, bytes)):
        return value.decode("utf-8") if isinstance(value, bytes) else value
    return _json_stdlib.dumps(value, ensure_ascii=False, separators=(",", ":"))


class DeviceClient:
    """某个 SN 的设备视图。

    由 `nclink.get_device(sn)` 创建，**不要自己 new**。同一个 SN 在库里只有一份
    客户端（进程级连接断开时统一回收），所以这里的 `close()` 只退订、清回调，
    不断开 MQTT —— 断开是 `nclink.shutdown()` 的事。
    """

    def __init__(self, sn, handle):
        self._sn = sn
        self._handle = handle
        self._subscribed_samples = False
        self._subscribed_events = False
        self._sample_callback = None
        self._event_callback = None
        self._sample_host = None      # (ctypes 回调, host 槽)：退订前不能被 GC
        self._event_host = None
        self.last_callback_error = None

    # ------------------------------------------------------------ 基本 -- #

    @property
    def sn(self):
        return self._sn

    def _check_open(self):
        if self._handle is None:
            raise NclinkError(-13, "DeviceClient", "客户端已关闭")
        return self._handle

    def __str__(self):
        return "DeviceClient(%s)" % self._sn

    __repr__ = __str__

    # ------------------------------------------------------------ 读值 -- #

    def probe(self, timeout_ms=5000):
        """探测设备，拿到模型（返回的 `Model` 用完要 close）。

        顺带把模型装进客户端（客户端自己留一份拷贝，见 `load_model()`），所以
        probe 之后 `get_id()` / `get_path()` 与采样报文按模型补齐 `paths` 都能用。
        """
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_probe(self._check_open(), int(timeout_ms),
                                      ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "probe")
        model = Model(out.value)
        self.load_model(model)
        return model

    def load_model(self, model):
        """把 `model` 装进客户端（`None` = 清掉当前模型）。

        客户端拿的是**自己的拷贝**，所以 `model` 还是调用方的，照常 close。
        """
        rc = lib.nclshim_client_set_root_node(
            self._check_open(), None if model is None else model._handle)
        if rc != 0:
            raise NclinkError(rc, "load_model")

    def get_value(self, path, timeout_ms=5000):
        """读值（返回的 `Json` 用完要 close）。"""
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_get_value(self._check_open(), encode(path),
                                          int(timeout_ms), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "get_value")
        return Json(out.value, owned=True)

    def get_value_range(self, path, start, end, timeout_ms=5000):
        """读一个区间（数组数据项）。"""
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_get_value_range(self._check_open(), encode(path),
                                                int(start), int(end), int(timeout_ms),
                                                ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "get_value_range")
        return Json(out.value, owned=True)

    def get_long(self, path, timeout_ms=5000, default=0):
        """读一个整数；值不是整数时（或读失败）给 default。"""
        with self.get_value(path, timeout_ms) as value:
            return value.as_long(default)

    def get_double(self, path, timeout_ms=5000, default=0.0):
        with self.get_value(path, timeout_ms) as value:
            return value.as_double(default)

    def get_length(self, path, timeout_ms=5000):
        """数组数据项的长度。"""
        out = ctypes.c_longlong()
        rc = lib.nclshim_client_get_length(self._check_open(), encode(path),
                                           int(timeout_ms), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "get_length")
        return int(out.value)

    # ------------------------------------------------------------ 写值 -- #

    def set_value(self, path, value, timeout_ms=5000):
        """写值；`value` 见 `_as_json_text()`（Json / JSON 文本 / Python 对象）。"""
        rc = lib.nclshim_client_set_value(self._check_open(), encode(path),
                                          encode(_as_json_text(value)), int(timeout_ms))
        if rc != 0:
            raise NclinkError(rc, "set_value")

    def set_value_index(self, path, value, index, timeout_ms=5000):
        """按索引写数组里的一个元素。"""
        rc = lib.nclshim_client_set_value_index(self._check_open(), encode(path),
                                                encode(_as_json_text(value)),
                                                int(index), int(timeout_ms))
        if rc != 0:
            raise NclinkError(rc, "set_value_index")

    def method_call(self, method, params=None, check=False, timeout_ms=5000):
        """方法调用；返回应答报文的 `Json`（code / params / data / reason）。

        `method` 形如 ``/plc/setValue``（``plc/setValue`` 也认），`check=True`
        只校验参数、不执行。
        """
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_method_call(
            self._check_open(), encode(method),
            None if params is None else encode(_as_json_text(params)),
            1 if check else 0, int(timeout_ms), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "method_call")
        # 方法调用返还的是**应答报文的 JSON 文本**，不是 JSON 句柄
        text = take_text(out.value)
        if text is None:
            raise NclinkError(-1, "method_call", "没有应答")
        return Json.parse(text)

    def ping(self, timeout_ms=5000):
        """心跳：Ping/<sn> -> Pong/<sn>。"""
        rc = lib.nclshim_client_ping(self._check_open(), int(timeout_ms))
        if rc != 0:
            raise NclinkError(rc, "ping")

    # -------------------------------------------------------- 异步方法调用 -- #

    def method_call_async(self, method, params=None, timeout_ms=5000):
        """异步方法调用：立刻回一个应答 `Json`（`code=OK` + `handler`），
        方法在设备端线程池里跑。拿 `handler` 去 `method_status()` /
        `method_result()` 查进度与结果。
        """
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_method_call_async(
            self._check_open(), encode(method),
            None if params is None else encode(_as_json_text(params)),
            int(timeout_ms), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "method_call_async")
        text = take_text(out.value)
        if text is None:
            raise NclinkError(-1, "method_call_async", "没有应答")
        return Json.parse(text)

    def method_status(self, object_id, handler, timeout_ms=5000):
        """按句柄查异步调用的状态：`process` / `status`(executing|waiting|
        stopped|sleep) / `code`。"""
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_method_status(self._check_open(),
                                              encode(object_id), encode(handler),
                                              int(timeout_ms), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "method_status")
        text = take_text(out.value)
        if text is None:
            raise NclinkError(-1, "method_status", "没有应答")
        return Json.parse(text)

    def method_result(self, object_id, handler, timeout_ms=5000):
        """按句柄查异步调用的结果：没跑完是 `code=PENDING`（没有 `result`），
        跑完是 `code` + `return` + `result`(finished|error)，同时句柄被释放。"""
        out = ctypes.c_void_p()
        rc = lib.nclshim_client_method_result(self._check_open(),
                                              encode(object_id), encode(handler),
                                              int(timeout_ms), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "method_result")
        text = take_text(out.value)
        if text is None:
            raise NclinkError(-1, "method_result", "没有应答")
        return Json.parse(text)

    # ------------------------------------------------------ 文件通道 -- #

    def open_file_channel(self, host=None, port=0, username=None,
                          password=None):
        """开文件通道（file/openFileChannel）：把托管侧的 FTP 端点交给设备，
        设备随即往那儿拨 FTP 传字节。

        参数全空就用默认：地址 = 到 broker 的本机地址（拿不到就 127.0.0.1）、
        端口 = 进程级端点端口（没起就按 2323 起）、账号 = 库临时生成的一对
        （关闭时撤销）。对端不在这台机器上、或端口有映射时给 `host` / `port` /
        `username` / `password`。

        幂等；通道是租约，`close_file_channel()` 之前一直是这条对端。上传/
        下载/列目录/建目录/删除这些便利方法也会在没通道时自动开一次。
        """
        if host or port or username or password:
            rc = lib.nclshim_client_file_channel_open_ex(
                self._check_open(), encode(host), int(port), encode(username),
                encode(password))
        else:
            rc = lib.nclshim_client_file_channel_open(self._check_open())
        if rc != 0:
            raise NclinkError(rc, "open_file_channel")

    def close_file_channel(self):
        """收回文件通道：设备停用它的 FTP 连接，库给这条通道加的账号一并撤销（幂等）。"""
        rc = lib.nclshim_client_file_channel_close(self._check_open())
        if rc != 0:
            raise NclinkError(rc, "close_file_channel")

    def file_channel_is_open(self):
        """这条客户端手上有没有文件通道。"""
        return bool(lib.nclshim_client_file_channel_is_open(self._check_open()))

    def upload_file(self, relative_path, timeout_ms=5000):
        """上传 `<当前目录>/<sn><相对路径>` 上的文件；`upload_local_file()` 会替你
        把本地文件摆到那个位置。"""
        return _file.upload_file(self, relative_path, timeout_ms)

    def upload_local_file(self, local_path, relative_path=None, timeout_ms=5000):
        """把本地文件传过去（相对路径省略时用本地文件名）。"""
        return _file.upload_local_file(self, local_path, relative_path, timeout_ms)

    def download_file(self, relative_path):
        """下载文件，返回落盘后的本地绝对路径（在 `<当前目录>/<sn>/` 下面）。"""
        return _file.download_file(self, relative_path)

    def download_to(self, relative_path, local_path):
        """下载并复制到 `local_path`；返回落点绝对路径。"""
        return _file.download_to(self, relative_path, local_path)

    def list_files(self, remote_dir="/"):
        """列设备上的目录，返回 `FileInfo` 列表。"""
        return _file.list_files(self, remote_dir)

    def make_directory(self, remote_dir):
        """在设备上建目录。"""
        return _file.make_directory(self, remote_dir)

    def delete_file(self, remote_path):
        """删设备上的文件或目录（目录递归删）。"""
        return _file.delete_file(self, remote_path)

    def method_call_file(self, method, params=None, keys=None, paths=None,
                         timeout_ms=5000):
        """带文件参数的方法调用（keys 与 paths 一一对应）。"""
        return _file.method_call_file(self, method, params, keys, paths,
                                      timeout_ms)

    def staged_path(self, relative_path):
        """文件通道的暂存位置：`<当前目录>/<sn><相对路径>`。"""
        return _file._staged_path(self.sn, relative_path)

    # ------------------------------------------------------- 路径 / id -- #

    def get_id(self, path):
        """数据项路径 -> 节点 id（没有就是 None）。"""
        return take_text(lib.nclshim_client_get_id(self._check_open(), encode(path)))

    def get_path(self, node_id):
        """节点 id -> 数据项路径（没有就是 None）。"""
        return take_text(lib.nclshim_client_get_path(self._check_open(), encode(node_id)))

    # ------------------------------------------------------------ 订阅 -- #

    def subscribe_samples(self, qos=0, callback=None):
        """订阅采样（``Sample/<sn>/#``）。

        `callback(topic, sample)` 在客户端自己的**读取线程**上调用，收到的
        `Sample` 已经是快照（出了回调也能用）；别在回调里做耗时操作。
        """
        if callback is not None:
            self._sample_callback = callback
        host = self._make_host(self._dispatch_sample)
        rc = lib.nclshim_client_subscribe_samples(self._check_open(), int(qos),
                                                  host[1])
        if rc != 0:
            raise NclinkError(rc, "subscribe_samples")
        self._sample_host = host
        self._subscribed_samples = True

    def unsubscribe_samples(self):
        """退订采样（回调也一并清掉）。"""
        if not self._subscribed_samples:
            return
        rc = lib.nclshim_client_unsubscribe_samples(self._check_open())
        self._subscribed_samples = False
        self._sample_host = None      # 垫片已经把挂在客户端上的回调摘了
        if rc != 0:
            raise NclinkError(rc, "unsubscribe_samples")

    def subscribe_events(self, qos=2, callback=None):
        """订阅事件（``Event/<sn>``）；`callback(topic, event)`，规则同上。"""
        if callback is not None:
            self._event_callback = callback
        host = self._make_host(self._dispatch_event)
        rc = lib.nclshim_client_subscribe_events(self._check_open(), int(qos), host[1])
        if rc != 0:
            raise NclinkError(rc, "subscribe_events")
        self._event_host = host
        self._subscribed_events = True

    def unsubscribe_events(self):
        """退订事件（回调也一并清掉）。"""
        if not self._subscribed_events:
            return
        rc = lib.nclshim_client_unsubscribe_events(self._check_open())
        self._subscribed_events = False
        self._event_host = None
        if rc != 0:
            raise NclinkError(rc, "unsubscribe_events")

    @property
    def sample_count(self):
        """收到过多少条采样（回调里累加，断线重连不清零）。"""
        return int(lib.nclshim_client_sample_count(self._check_open()))

    @property
    def event_count(self):
        """收到过多少条事件。"""
        return int(lib.nclshim_client_event_count(self._check_open()))

    # ------------------------------------------------------- 运行时通道 -- #

    def add_sample(self, config, timeout_ms=5000):
        """运行时加一个采样通道；`config` 是一个 SAMPLE_CHANNEL 配置节点。"""
        rc = lib.nclshim_client_add_sample(self._check_open(),
                                           encode(_as_json_text(config)),
                                           int(timeout_ms))
        if rc != 0:
            raise NclinkError(rc, "add_sample")

    def remove_sample(self, channel_id, timeout_ms=5000):
        """运行时删掉一个采样通道。"""
        rc = lib.nclshim_client_remove_sample(self._check_open(),
                                              encode(channel_id), int(timeout_ms))
        if rc != 0:
            raise NclinkError(rc, "remove_sample")

    # ---------------------------------------------------------- 收尾 -- #

    def close(self):
        """退订采样/事件、清回调（可重复调用）。"""
        if self._handle is None:
            return
        try:
            self.unsubscribe_samples()
            self.unsubscribe_events()
        except NclinkError:
            pass                          # 收尾时出错不往外抛
        self._sample_host = None
        self._event_host = None
        self._handle = None
        self._sample_callback = None
        self._event_callback = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    # -------------------------------------------------------- 回调桥 -- #

    def _make_host(self, dispatch):
        """垫片要的 host 是两格数组 [函数指针, 用户数据]。

        `dispatch(user, topic, msg)` 是 Python 侧的转发；ctypes 回调对象与它的
        host 槽一起记在返回的那一项上，**退订之前必须一直被引用**，否则回调跳板
        被回收，C 侧再跳到那里就是野指针。
        """
        callback = MESSAGE_CALLBACK(dispatch)
        slots = (ctypes.c_void_p * 2)()
        slots[0] = ctypes.cast(callback, ctypes.c_void_p)
        slots[1] = None
        return (callback, ctypes.cast(slots, ctypes.c_void_p))

    def _dispatch_sample(self, user, topic, msg):
        callback = self._sample_callback
        if callback is None:
            return
        try:
            callback(decode(topic), Sample.capture(msg, decode(topic)))
        except Exception as exc:      # 回调里抛异常不能穿回 C 侧线程
            self.last_callback_error = exc

    def _dispatch_event(self, user, topic, msg):
        callback = self._event_callback
        if callback is None:
            return
        try:
            callback(decode(topic), Event.capture(msg, decode(topic)))
        except Exception as exc:
            self.last_callback_error = exc
