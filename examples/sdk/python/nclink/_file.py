# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""文件通道：MQTT 报文里只传 "/temp/<名字>" 令牌，字节走 FTP。

方向很重要：**设备是 FTP 客户端**，本机是 FTP 服务端（进程级端点
127.0.0.1:2323，admin / 123456，根 = 安装根；开文件通道时按需起）。

传字节之前要先握手：`DeviceClient.open_file_channel()` 把端点交给设备
（file/openFileChannel），设备随即往那儿拨 FTP。下面的便利方法会自己确保通道
开着；`DeviceClient.close_file_channel()` 收回租约并撤销临时账号。

* 上传：文件先落到 `<当前目录>/<sn>/<相对路径>`（`DeviceClient.upload_file()`
  就是用这个约定；`upload_local_file()` 会替你摆好），
* 下载：字节先落到那儿，`download_file()` 返回绝对路径（`download_to()` 再替你
  搬到目标）。

设备侧要先把文件工具挂起来：`Server.register_file_tool()`（设备自己也服务 FTP 时
再加 `Server.start_ftp()`；不握手、钉静态对端则是 `Server.set_file_peer()`）。
"""

from __future__ import annotations

import ctypes
import json as _json_stdlib
import os
import shutil

from ._ffi import NclinkError, encode, lib, take_text
from ._json import Json

__all__ = ["FileInfo"]


class FileInfo:
    """远端一个文件或目录的属性（字段顺序按规范）。"""

    __slots__ = ("file_name", "file_type", "file_size", "total_chunks",
                 "compressed", "checksum", "parent_dir", "modify_time")

    def __init__(self, file_name=None, file_type=0, file_size=0, total_chunks=0,
                 compressed=False, checksum=None, parent_dir=None,
                 modify_time=0):
        self.file_name = file_name
        self.file_type = int(file_type)
        self.file_size = int(file_size)
        self.total_chunks = int(total_chunks)
        self.compressed = bool(compressed)
        self.checksum = checksum
        self.parent_dir = parent_dir
        self.modify_time = int(modify_time)

    @property
    def is_dir(self):
        return self.file_type == 1

    @classmethod
    def from_json(cls, value):
        """从属性 JSON 对象（dict 或 `Json`）建一个。"""
        if isinstance(value, Json):
            value = value.to_python()
        return cls(file_name=value.get("fileName"),
                   file_type=value.get("fileType", 0),
                   file_size=value.get("fileSize", 0),
                   total_chunks=value.get("totalChunks", 0),
                   compressed=value.get("compressed", False),
                   checksum=value.get("checksum"),
                   parent_dir=value.get("parantDir"),
                   modify_time=value.get("modifyTime", 0))

    @classmethod
    def parse_list(cls, text):
        """把 `nclshim_client_file_ll_json()` 那种 JSON 文本解析成列表。"""
        if not text:
            return []
        with Json.parse(text) as array:
            return [cls.from_json(item) for item in array.to_python()]

    def __str__(self):
        if self.is_dir:
            return "%s/ (目录)" % self.file_name
        return "%s (%d 字节，%d 片%s)" % (
            self.file_name, self.file_size, self.total_chunks,
            "，压缩" if self.compressed else "")

    __repr__ = __str__


# ------------------------------------------------------------ 客户端的文件通道 -- #

def _staged_path(sn, relative_path):
    """`<当前目录>/<sn><相对路径>`（与 C API 的约定一致）。"""
    root = os.path.join(os.getcwd(), sn)
    if not relative_path:
        return root
    tail = relative_path.replace("\\", "/").lstrip("/")
    return os.path.join(root, *tail.split("/"))


def upload_file(client, relative_path, timeout_ms=5000):
    """上传 `<当前目录>/<sn><相对路径>` 上的文件（见 `upload_local_file`）。"""
    rc = lib.nclshim_client_file_write(client._check_open(), encode(relative_path))
    if rc != 0:
        raise NclinkError(rc, "upload_file")


def upload_local_file(client, local_path, relative_path=None, timeout_ms=5000):
    """把本地文件摆到暂存位置再上传；`relative_path` 省略时用本地文件名。"""
    if relative_path is None:
        relative_path = "/" + os.path.basename(local_path)
    staged = _staged_path(client.sn, relative_path)
    parent = os.path.dirname(staged)
    if parent:
        os.makedirs(parent, exist_ok=True)
    if os.path.abspath(local_path) != os.path.abspath(staged):
        shutil.copyfile(local_path, staged)
    upload_file(client, relative_path, timeout_ms)


def download_file(client, relative_path):
    """下载文件，返回落盘后的本地绝对路径。"""
    local = take_text(lib.nclshim_client_file_read(client._check_open(),
                                                   encode(relative_path)))
    if not local:
        raise NclinkError(-1, "download_file", "下载失败: %s" % relative_path)
    return local


def download_to(client, relative_path, local_path):
    """下载并复制到 `local_path`；返回落点绝对路径。"""
    staged = download_file(client, relative_path)
    parent = os.path.dirname(local_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    shutil.copyfile(staged, local_path)
    return os.path.abspath(local_path)


def list_files(client, remote_dir="/"):
    """列设备上的目录，返回 `FileInfo` 列表。"""
    text = take_text(lib.nclshim_client_file_ll_json(client._check_open(),
                                                     encode(remote_dir)))
    if text is None:
        raise NclinkError(-1, "list_files", "列目录失败: %s" % remote_dir)
    return FileInfo.parse_list(text)


def make_directory(client, remote_dir):
    rc = lib.nclshim_client_file_mkdir(client._check_open(), encode(remote_dir))
    if rc != 0:
        raise NclinkError(rc, "make_directory")


def delete_file(client, remote_path):
    rc = lib.nclshim_client_file_delete(client._check_open(), encode(remote_path))
    if rc != 0:
        raise NclinkError(rc, "delete_file")


def method_call_file(client, method, params=None, keys=None, paths=None,
                     timeout_ms=5000):
    """带文件参数的方法调用（keys 与 paths 一一对应；应答里的 fileKeys 会被
    换成本地路径）。"""
    from .client import _as_json_text

    text = None if params is None else _as_json_text(params)
    out = ctypes.c_void_p()
    rc = lib.nclshim_client_method_call_file(
        client._check_open(), encode(method), encode(text),
        encode(_json_stdlib.dumps(keys or [], ensure_ascii=False)),
        encode(_json_stdlib.dumps(paths or [], ensure_ascii=False)),
        int(timeout_ms), ctypes.byref(out))
    if rc != 0:
        raise NclinkError(rc, "method_call_file")
    reply = take_text(out)
    if reply is None:
        raise NclinkError(-1, "method_call_file", "没有应答")
    return Json.parse(reply)
