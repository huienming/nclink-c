# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""ctypes 桥：找 nclink_shim 共享库、声明函数原型、把错误码变成异常。

和 C# 绑定一样，Python 侧**不直接**碰 C 结构体：所有调用都过
``bindings/native/nclink_shim.c`` 那层扁平的 C ABI（不透明句柄 + 标量 + UTF-8
文本）。字符串统一按 UTF-8 编解码，中文不会走系统的 ANSI 代码页。

找库的顺序（第一个存在的就用）：

1. 环境变量 ``NCLINK_SHIM`` 指向的文件；
2. 本包目录（把 nclink_shim.dll / libnclink_shim.so 放在 ``nclink/`` 旁边）；
3. 仓库布局：``bindings/native/bin/``、``build/``、``build/bin/``、``build-linux/``；
4. 交给系统：``ctypes.util.find_library("nclink_shim")`` 与裸名字（PATH /
   LD_LIBRARY_PATH）。
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys

__all__ = ["lib", "NclinkError", "check", "decode", "encode", "version"]


class NclinkError(Exception):
    """库返回的非 0 错误码。``code`` 是 ncl_err，``op`` 是出错的调用名。"""

    def __init__(self, code, op="", detail=""):
        self.code = int(code)
        self.op = op
        self.name = _err_name(self.code)
        self.detail = detail
        message = self.name if not op else "%s: %s" % (op, self.name)
        if detail:
            message = "%s（%s）" % (message, detail)
        super().__init__(message)


def _err_name(code):
    try:
        return decode(lib.nclshim_err_name(code)) or ("err-%d" % code)
    except Exception:  # pragma: no cover - 库还没加载好时的兜底
        return "err-%d" % code


def encode(text):
    """str -> UTF-8 bytes；None 原样传（C 侧就是 NULL）。"""
    if text is None:
        return None
    if isinstance(text, bytes):
        return text
    return str(text).encode("utf-8")


def decode(raw):
    """C 的 const char* -> str（NULL 得 None）。"""
    if raw is None:
        return None
    if isinstance(raw, str):
        return raw
    if isinstance(raw, bytes):
        return raw.decode("utf-8", "replace")
    return ctypes.cast(raw, ctypes.c_char_p).value.decode("utf-8", "replace")


def take_text(raw):
    """接一份 malloc 出来的 char*：转成 str 后立刻 nclshim_free。"""
    if not raw:
        return None
    text = decode(raw)
    lib.nclshim_free(raw)
    return text


def check(rc, op=""):
    """rc != 0 就抛 NclinkError。"""
    if rc != 0:
        raise NclinkError(rc, op)
    return 0


# ------------------------------------------------------------------ 加载 -- #

_LIB_NAMES = (
    "nclink_shim.dll",
    "libnclink_shim.so",
    "libnclink_shim.dylib",
    "nclink_shim",
)


def _candidates():
    override = os.environ.get("NCLINK_SHIM")
    if override:
        yield override
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(os.path.dirname(here)))  # <repo>/bindings/python/nclink
    roots = [
        here,                                   # 包目录（pip 安装 / 手工拷贝）
        os.path.join(repo, "bindings", "native", "bin"),
        os.path.join(repo, "build"),
        os.path.join(repo, "build", "bin"),
        os.path.join(repo, "build-linux"),
        os.path.join(repo, "build-linux", "bin"),
    ]
    for root in roots:
        for name in _LIB_NAMES:
            yield os.path.join(root, name)
    found = ctypes.util.find_library("nclink_shim")
    if found:
        yield found
    for name in _LIB_NAMES:
        yield name


def _load():
    problems = []
    for candidate in _candidates():
        path = candidate
        if os.sep in candidate or (os.altsep and os.altsep in candidate):
            if not os.path.isfile(candidate):
                continue
        try:
            return ctypes.CDLL(path)
        except OSError as exc:      # 文件在但架构/依赖不对
            problems.append("%s: %s" % (path, exc))
    detail = ("\n  " + "\n  ".join(problems)) if problems else ""
    raise ImportError(
        "找不到 nclink_shim（nclink_shim.dll / libnclink_shim.so）。"
        "先在仓库根跑 .\\build.ps1，再跑 "
        "powershell -ExecutionPolicy Bypass -File .\\bindings\\native\\build-shim.ps1；"
        "或者用环境变量 NCLINK_SHIM 指向它。" + detail
    )


lib = _load()


# ------------------------------------------------------------- 原型声明 -- #

# (名字, 返回类型, 参数类型...)：只声明 Python 这一层用得到的。
_PROTOTYPES = [
    ("nclshim_version", ctypes.c_char_p),
    ("nclshim_err_name", ctypes.c_char_p, ctypes.c_int),
    ("nclshim_free", None, ctypes.c_void_p),
    ("nclshim_strdup", ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_env_set_root", None, ctypes.c_char_p),
    ("nclshim_env_root", ctypes.c_char_p),
    ("nclshim_log_init", ctypes.c_int, ctypes.c_char_p),
    ("nclshim_log_shutdown", None),
    ("nclshim_log_set_level", None, ctypes.c_int),
    ("nclshim_log_set_console", None, ctypes.c_int),
    # json
    ("nclshim_json_parse", ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_json_clone", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_json_free", None, ctypes.c_void_p),
    ("nclshim_json_write", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_json_type", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_json_is_null", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_json_as_int", ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_longlong)),
    ("nclshim_json_as_double", ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_double)),
    ("nclshim_json_as_bool", ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)),
    ("nclshim_json_string", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_json_text", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_json_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_json_array_get", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_json_object_value_at", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_json_object_key_at", ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_json_object_get", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    # model / node
    ("nclshim_model_parse", ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_model_free", None, ctypes.c_void_p),
    ("nclshim_model_write", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_model_find_by_id", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_node_type", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_node_type_name", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_name", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_id", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_path", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_description", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_number", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_data_type", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_value_type", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_mapping", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_source", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_version", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_guid", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_node_settable", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_node_value", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_node_is_sample_channel", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_node_sample_interval", ctypes.c_longlong, ctypes.c_void_p),
    ("nclshim_node_upload_interval", ctypes.c_longlong, ctypes.c_void_p),
    ("nclshim_node_sample_item_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_node_sample_item_path", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_node_count", ctypes.c_int, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_node_at", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int),
    # message / sample / event
    ("nclshim_message_parse", ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_message_free", None, ctypes.c_void_p),
    ("nclshim_message_write", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_message_type", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_sample_rows", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_sample_columns", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_sample_is_complete", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_sample_id", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_sample_begin_time", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_sample_interval", ctypes.c_longlong, ctypes.c_void_p),
    ("nclshim_sample_upload_interval", ctypes.c_longlong, ctypes.c_void_p),
    ("nclshim_sample_path", ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_sample_header", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_sample_column_slots", ctypes.c_int, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_sample_column_points", ctypes.c_int, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_sample_column_nested", ctypes.c_int, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_sample_column_encoding", ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_sample_value_at", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int),
    ("nclshim_sample_column_value_at", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int),
    ("nclshim_event_id", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_event_time", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_event_key", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_event_value", ctypes.c_void_p, ctypes.c_void_p),
    # client
    ("nclshim_open", ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p),
    ("nclshim_close", None),
    ("nclshim_is_open", ctypes.c_int),
    ("nclshim_client_get", ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_probe", ctypes.c_int, ctypes.c_void_p, ctypes.c_uint,
     ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_client_get_value", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_client_get_value_range", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_client_set_root_node", ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_client_get_length", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_uint, ctypes.POINTER(ctypes.c_longlong)),
    ("nclshim_client_set_value", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_uint),
    ("nclshim_client_set_value_index", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_int, ctypes.c_uint),
    ("nclshim_client_method_call", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_int, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_client_ping", ctypes.c_int, ctypes.c_void_p, ctypes.c_uint),
    ("nclshim_client_get_id", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_get_path", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_subscribe_samples", ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
     ctypes.c_void_p),
    ("nclshim_client_unsubscribe_samples", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_client_subscribe_events", ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
     ctypes.c_void_p),
    ("nclshim_client_unsubscribe_events", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_client_sample_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_client_event_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_client_add_sample", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_uint),
    ("nclshim_client_remove_sample", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_uint),
    # server（设备端）
    ("nclshim_server_create", ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_server_free", None, ctypes.c_void_p),
    ("nclshim_server_sn", ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_server_model", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_server_model_json", ctypes.c_void_p, ctypes.c_void_p),
    ("nclshim_server_binding_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_operation_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_sample_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_sample_upload_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_event_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_openapi_json", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_server_subscribe", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_register_tool", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p),
    ("nclshim_server_register_builtin_tool", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_register_file_tool", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_start_ftp", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_dispatch", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_server_invoke_method_call", ctypes.c_int, ctypes.c_void_p,
     ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_server_check_method_call", ctypes.c_int, ctypes.c_void_p,
     ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_server_init_samples", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_server_add_sample", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_server_remove_sample", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_server_stop_all_samples", None, ctypes.c_void_p),
    ("nclshim_server_push_event", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p),
    ("nclshim_server_push_event_ex", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_longlong, ctypes.c_char_p),
    # HTTP / REST
    ("nclshim_http_start", ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_http_free", None, ctypes.c_void_p),
    ("nclshim_http_port", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_http_request_count", ctypes.c_int, ctypes.c_void_p),
    ("nclshim_http_set_cors", None, ctypes.c_void_p, ctypes.c_int),
    ("nclshim_http_route", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p, ctypes.c_void_p),
    # 文件通道（MQTT 只传令牌，字节走 FTP）
    ("nclshim_file_start_ftp", ctypes.c_int),
    ("nclshim_file_stop_ftp", None),
    ("nclshim_client_file_write", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_file_read", ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_file_ll_json", ctypes.c_void_p, ctypes.c_void_p,
     ctypes.c_char_p),
    ("nclshim_client_file_mkdir", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_file_delete", ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_client_method_call_file", ctypes.c_int, ctypes.c_void_p,
     ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
     ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)),
    ("nclshim_file_need_compression", ctypes.c_int, ctypes.c_char_p),
    ("nclshim_file_total_chunks", ctypes.c_int, ctypes.c_longlong),
    ("nclshim_file_checksum", ctypes.c_void_p, ctypes.c_char_p),
    ("nclshim_file_attribute_json", ctypes.c_void_p, ctypes.c_char_p,
     ctypes.c_char_p),
]


def _declare():
    for entry in _PROTOTYPES:
        name, restype = entry[0], entry[1]
        func = getattr(lib, name)
        func.restype = restype
        func.argtypes = list(entry[2:])


_declare()

# 回调：void (*)(void *user, const char *topic, const void *msg)
MESSAGE_CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.c_void_p)

# 设备端工具回调：int (*)(user, tool, method, params, char **out_json, char **out_reason)
TOOL_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
                                 ctypes.c_char_p, ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.c_void_p),
                                 ctypes.POINTER(ctypes.c_void_p))

# 自研传输的发布回调：int (*)(user, topic, payload, len)
PUBLISH_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.c_void_p, ctypes.c_int)

# 自定义 HTTP 路由：int (*)(user, method, path, query, body,
#                           int *out_status, char **out_type, char **out_body)
ROUTE_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
                                  ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                  ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.POINTER(ctypes.c_void_p))


def strdup(text):
    """把 str/bytes 复制成垫片堆上的 C 字符串（回调回填字符串时必须用它）。

    返回 c_void_p；用完不再需要调用方释放 —— 垫片接手后自己 free。
    """
    return lib.nclshim_strdup(encode(text))


def version():
    """C 库版本号（例如 "3.1.0"）。"""
    return decode(lib.nclshim_version())
