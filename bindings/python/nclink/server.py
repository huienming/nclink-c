# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""设备端（ncl_server）：注册工具、路径绑定、采样任务、事件推送。

一个 Python 进程就是一台机床：

    import nclink

    device = nclink.Server(sn="V2023A7B762", broker="tcp://127.0.0.1:1883")
    device.register_tool("plc", methods={"getCount": None},
                         bindings=[("/MACHINE/PART_COUNT", nclink.Operation.GET_VALUE, "getCount")])
    device.subscribe()          # 订阅 6 个请求主题
    device.init_samples()       # 启动模型里声明的采样通道
    device.push_event("010307", {"key": "PART_COUNT", "value": 7})
    ...
    device.close()

不接 broker 也能用（`broker=None`）：此时用 `dispatch()` 离线驱动请求，或者给
`publish=` 一个回调自己当传输。工具方法回调跑在库自己的线程池上（不是 MQTT
读取线程），所以回调里可以放心调用库的其它接口。
"""

from __future__ import annotations

import ctypes
import json as _json_stdlib
import weakref
from enum import IntEnum

def _as_tls_options(tls):
    """dict / None / TlsOptions 都收（延迟 import 避免与包 __init__ 循环）。"""
    if tls is None or not isinstance(tls, dict):
        return tls
    from . import TlsOptions

    return TlsOptions(**tls)

from ._ffi import (NclinkError, PUBLISH_CALLBACK, ROUTE_CALLBACK, TOOL_CALLBACK,
                   decode, encode, lib, strdup, take_text)
from ._json import Json
from ._message import Message
from ._model import Model

__all__ = ["Server", "Operation", "HttpEndpoint"]

# ncl_err 里绑定用得到的几个（其余按名字见 nclink.NclinkError.name）
ERR = -1                 # 通用失败
ERR_NOT_FOUND = -6       # 方法/路径没注册
ERR_INVALID_ARG = -9
ERR_PARSE = -3


class Operation(IntEnum):
    """ncl_operation：路径绑定里的操作（"<operation>#<path>"）。"""

    GET_VALUE = 0
    GET_LENGTH = 1
    GET_KEYS = 2
    GET_ATTRIBUTES = 3
    SET_VALUE = 4
    ADD = 5
    DELETE = 6
    FUNC_CALL = 7
    FUNC_STATUS = 8
    FUNC_RESULT = 9
    FUNC_CANCEL = 10


def _json_body(value):
    """Python 对象 -> 紧凑 JSON 文本（bytes）。None 表示"没有值"。"""
    if isinstance(value, Json):
        return value.encode().encode("utf-8")
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode("utf-8")          # 字符串按 JSON 文本处理
    return _json_stdlib.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


class Server:
    """设备端。用完 `close()`（或 `with`）。"""

    def __init__(self, sn, model=None, broker=None, username=None, password=None,
                 publish=None, tls=None):
        """
        `sn`：设备序列号（必填，做 MQTT clientId 与主题地址）。

        `model`：模型；None = 库内置默认模型，也可以给 `Model` 或模型 JSON 文本。

        `broker`：MQTT 地址（如 tcp://127.0.0.1:1883）。None = 不接 MQTT（离线）。

        `publish`：可选的自研传输：`publish(topic: str, payload: bytes)`，给了它
        就不再走 MQTT 发布（但 `broker` 仍可用于收请求）。

        `tls`：`TlsOptions`（或同名字段的 dict），只在 `broker` 是 `ssl://` /
        `tls://` 时用得上（要带 TLS 编译的库与垫片，见 `nclink.tls_available()`）。
        """
        if not sn:
            raise ValueError("sn 不能为空")

        model_text = None
        if isinstance(model, Model):
            model_text = model.to_json()
        elif model is not None:
            model_text = str(model)
        else:
            # 设备端没有模型就没法做路径/采样：默认给库内置的那份（NC_LINK_ROOT）
            model_text = Model.parse().to_json()

        self._sn = sn
        self._handle = None
        self._tools = []
        self._handlers = {}
        self._http = []
        self._publish_callback = None
        self._publish_host = None
        self.last_callback_error = None

        publish_host = None
        if publish is not None:
            def forward(user, topic, payload, length):
                try:
                    publish(decode(topic), ctypes.string_at(payload, int(length)))
                except Exception as exc:                 # 不能穿回 C
                    self.last_callback_error = exc
                return 0

            self._publish_callback = PUBLISH_CALLBACK(forward)
            publish_host = self._host_for(self._publish_callback)
            self._publish_host = publish_host

        tls = _as_tls_options(tls)
        if tls is None:
            handle = lib.nclshim_server_create(encode(sn), encode(model_text),
                                               encode(broker), encode(username),
                                               encode(password), publish_host)
        else:
            handle = lib.nclshim_server_create_ex(
                encode(sn), encode(model_text), encode(broker), encode(username),
                encode(password), encode(tls.ca_file), encode(tls.client_cert),
                encode(tls.client_key), encode(tls.server_name),
                1 if tls.verify_peer else 0, publish_host)
        if not handle:
            raise NclinkError(ERR_NOT_FOUND, "Server",
                              "创建设备端失败（broker 连不上？）: %s" % sn)
        self._handle = handle

    # ------------------------------------------------------------ 基本 -- #

    @property
    def sn(self):
        return self._sn

    @property
    def model(self):
        """设备模型（借用视图：服务器活着它就有效，不用关）。"""
        handle = lib.nclshim_server_model(self._require_open())
        if not handle:
            return None
        return Model._borrowed(handle, self)

    def model_json(self):
        return take_text(lib.nclshim_server_model_json(self._require_open()))

    def methods(self):
        """这台设备现在能调用什么：模型 METHODS 项的 value，每条一个 dict。

        字段：``tool`` / ``method`` / ``address``（methodCall 里写这个）/
        ``params`` / ``result`` / ``bindings``（后三项没有就不出现）。
        和客户端 probe 回来的那份一模一样（见 `DeviceClient.methods`）。
        """
        out = ctypes.c_void_p()
        rc = lib.nclshim_server_methods_json(self._require_open(), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "methods")
        text = take_text(out.value)
        return _json_stdlib.loads(text) if text else []

    @property
    def binding_count(self):
        """已注册的 "<operation>#<path>" 绑定数（每个方法名本身也算一条）。"""
        return int(lib.nclshim_server_binding_count(self._require_open()))

    @property
    def operation_count(self):
        """可调用的 (工具, 方法) 对数。"""
        return int(lib.nclshim_server_operation_count(self._require_open()))

    @property
    def sample_count(self):
        """已注册的采样通道数。"""
        return int(lib.nclshim_server_sample_count(self._require_open()))

    @property
    def sample_upload_count(self):
        """采样上报次数（诊断用）。"""
        return int(lib.nclshim_server_sample_upload_count(self._require_open()))

    @property
    def event_count(self):
        """已发布事件数。"""
        return int(lib.nclshim_server_event_count(self._require_open()))

    def openapi_json(self, base_url=""):
        """OpenAPI 3.0 文档（每个 <工具>/<方法> 一个 POST 路径）。"""
        return take_text(lib.nclshim_server_openapi_json(self._require_open(),
                                                         encode(base_url)))

    def _require_open(self):
        if self._handle is None:
            raise NclinkError(-13, "Server", "设备端已关闭")
        return self._handle

    # ------------------------------------------------------------ 工具 -- #

    def register_tool(self, tool, methods, bindings=None, handlers=None):
        """注册一个工具。

        `tool`：工具名（形如 "plc"）。

        `methods`：`{方法名: schema}`（schema 是 JSON Schema：dict / JSON 文本 /
        None，用来支持 `check` 只校验不执行），也可以给 `["getValue", ...]` 这样
        的名字列表。

        `bindings`：`[(路径, Operation, 方法名), ...]`，也可以给
        `{"path":..., "operation":..., "method":...}` 的列表；路径是模型里的完整
        路径（如 "/MACHINE/STATUS"）。

        `handlers`：`{方法名: 处理函数}`；不传就用 `server.handlers` 里先放好的。
        处理函数收 `params`（Python 对象或 None），返回要应答的值：返回 None 表示
        "没有值"（库按 NG 应答），抛异常则按错误码 + 异常文本应答。
        """
        if isinstance(methods, dict):
            method_list = [{"name": name, "schema": schema}
                           for name, schema in methods.items()]
        else:
            method_list = [{"name": entry} if isinstance(entry, str) else dict(entry)
                           for entry in methods]

        binding_list = []
        for spec in (bindings or []):
            if isinstance(spec, dict):
                path, op, method = spec["path"], spec["operation"], spec["method"]
            else:
                path, op, method = spec
            binding_list.append({"path": path, "operation": int(op), "method": method})

        resolved = dict(self._handlers)
        if handlers:
            resolved.update(handlers)
        bound = {}
        for entry in method_list:
            name = entry["name"]
            handler = entry.get("handler")
            if handler is None:
                handler = resolved.get(name)
            if handler is None:
                raise ValueError("方法 %s 没有处理函数（先设 server.handlers[%r] "
                                 "或用 handlers= 传进来）" % (name, name))
            bound[name] = handler

        def dispatch(user, tool_name, method, params, out_json, out_reason):
            try:
                return self._call_handler(bound, decode(tool_name), decode(method),
                                          params, out_json, out_reason)
            except Exception as exc:                 # 兜底：绝不让异常穿回 C
                self.last_callback_error = exc
                if out_reason:
                    out_reason[0] = strdup(str(exc))
                return ERR

        callback = TOOL_CALLBACK(dispatch)
        host = self._host_for(callback)
        rc = lib.nclshim_server_register_tool(
            self._require_open(), encode(tool),
            encode(_json_stdlib.dumps(method_list, ensure_ascii=False)),
            encode(_json_stdlib.dumps(binding_list, ensure_ascii=False) if binding_list else None),
            host)
        if rc != 0:
            raise NclinkError(rc, "register_tool", tool)
        self._tools.append((callback, host))         # 回调要活到 close()

    @property
    def handlers(self):
        """`{方法名: 处理函数}`：注册时可以只写方法名，处理函数放这里。"""
        return self._handlers

    def _call_handler(self, handlers, tool, method, params, out_json, out_reason):
        handler = handlers.get(method)
        if handler is None:
            if out_reason:
                out_reason[0] = strdup("没有注册方法 %s" % method)
            return ERR_NOT_FOUND
        params_value = None if not params else Json(params).to_python()
        try:
            result = handler(params_value)
        except NclinkError as exc:
            self.last_callback_error = exc          # 记下来但不穿回 C
            if out_reason:
                out_reason[0] = strdup(str(exc))
            return exc.code
        except Exception as exc:
            self.last_callback_error = exc
            if out_reason:
                out_reason[0] = strdup(str(exc))
            return ERR
        if result is None:
            return 0                                  # 成功但没有值 → 库按 NG 应答
        if out_json is None:
            return ERR
        out_json[0] = strdup(_json_body(result))
        return 0

    def register_builtin_tool(self):
        """注册内置的 "nclinkServer" 工具（addSample / removeSample）。"""
        rc = lib.nclshim_server_register_builtin_tool(self._require_open())
        if rc != 0:
            raise NclinkError(rc, "register_builtin_tool")

    def register_file_tool(self):
        """注册文件工具（/CONTROLLER/FILE，配 start_ftp 使用）。"""
        rc = lib.nclshim_server_register_file_tool(self._require_open())
        if rc != 0:
            raise NclinkError(rc, "register_file_tool")

    def set_file_peer(self, host, port=2323, username=None, password=None):
        """覆盖文件通道对端的 FTP 端点。

        默认按 `conf/mqtt.cfg` 推：broker 的主机名 + 端口 2323 + admin/123456 ——
        只有对端跑在 broker 那台机器上才成立。对端在别处（或者端口/账号不一样）时
        在**第一次传文件之前**调它；`port=0`、账号留空就用默认值。
        """
        if not host:
            raise ValueError("host 不能为空")
        rc = lib.nclshim_server_set_file_peer(self._require_open(), encode(host),
                                              int(port), encode(username),
                                              encode(password))
        if rc != 0:
            raise NclinkError(rc, "set_file_peer")

    def start_ftp(self):
        """启动 FTP 端点（端口与账号来自 bin/ftp.txt）。"""
        rc = lib.nclshim_server_start_ftp(self._require_open())
        if rc != 0:
            raise NclinkError(rc, "start_ftp")

    def subscribe(self):
        """订阅本 SN 的 6 个请求主题（接 MQTT 时用）。"""
        rc = lib.nclshim_server_subscribe(self._require_open())
        if rc != 0:
            raise NclinkError(rc, "subscribe")

    # ------------------------------------------------------------ 离线 -- #

    def dispatch(self, topic, payload):
        """离线驱动一条请求，返回应答报文（`Message` / `Sample` / `Event`）。

        不经过 MQTT、也不发布应答；`payload` 是原始报文体（bytes / str）。
        """
        data = payload if isinstance(payload, (bytes, bytearray)) else str(payload).encode("utf-8")
        data = bytes(data)
        buffer = ctypes.create_string_buffer(data, max(len(data), 1))
        out = ctypes.c_void_p()
        rc = lib.nclshim_server_dispatch(self._require_open(), encode(topic),
                                         ctypes.cast(buffer, ctypes.c_void_p),
                                         len(data), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "dispatch")
        return _parse_response(_response_topic(topic), take_text(out))

    def invoke_method_call(self, method, params=None):
        """离线调用一个工具方法（不经过 MQTT），返回应答报文。"""
        return self._call(method, params, False)

    def check_method_call(self, method, params=None):
        """只按 schema 校验参数、不执行工具，返回应答报文。"""
        return self._call(method, params, True)

    def invoke_method_call_async(self, method, params=None):
        """离线发起一次异步方法调用：应答 code=OK + handler（方法在池里跑），
        随后用 method_status()/method_result() 按句柄查。"""
        text = None if params is None else _json_stdlib.dumps(params, ensure_ascii=False)
        out = ctypes.c_void_p()
        rc = lib.nclshim_server_invoke_method_call_async(
            self._require_open(), encode(method), encode(text), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "invoke_method_call_async")
        return _parse_response("Method/Call/Response/%s" % self._sn, take_text(out))

    def invoke_method_status(self, object_id, handler):
        """按句柄查状态（离线；真机上由 Method/Status/Request 触发）。"""
        out = ctypes.c_void_p()
        rc = lib.nclshim_server_invoke_method_status(
            self._require_open(), encode(object_id), encode(handler),
            ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "invoke_method_status")
        return _parse_response("Method/Status/Response/%s" % self._sn,
                               take_text(out))

    def invoke_method_result(self, object_id, handler):
        """按句柄查结果（离线；真机上由 Method/Result/Request 触发）。"""
        out = ctypes.c_void_p()
        rc = lib.nclshim_server_invoke_method_result(
            self._require_open(), encode(object_id), encode(handler),
            ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "invoke_method_result")
        return _parse_response("Method/Result/Response/%s" % self._sn,
                               take_text(out))

    def report_method_progress(self, handler, process, status=None):
        """给正在跑的异步调用上报进度（可选）。"""
        rc = lib.nclshim_server_report_method_progress(
            self._require_open(), encode(handler), int(process), encode(status))
        if rc != 0:
            raise NclinkError(rc, "report_method_progress")

    def _call(self, method, params, check):
        text = None if params is None else _json_stdlib.dumps(params, ensure_ascii=False)
        out = ctypes.c_void_p()
        func = (lib.nclshim_server_check_method_call if check
                else lib.nclshim_server_invoke_method_call)
        rc = func(self._require_open(), encode(method), encode(text), ctypes.byref(out))
        if rc != 0:
            raise NclinkError(rc, "check_method_call" if check else "invoke_method_call")
        return _parse_response("Method/Call/Response/%s" % self._sn, take_text(out))

    # ------------------------------------------------------------ 采样 -- #

    def init_samples(self):
        """启动模型里声明的采样通道（模型的第一个设备下的 SAMPLE_CHANNEL）。"""
        rc = lib.nclshim_server_init_samples(self._require_open())
        if rc != 0:
            raise NclinkError(rc, "init_samples")

    def add_sample(self, config):
        """运行时加一个采样通道（config 是 SAMPLE_CHANNEL 配置节点）。"""
        rc = lib.nclshim_server_add_sample(self._require_open(),
                                           _json_body(config))
        if rc != 0:
            raise NclinkError(rc, "add_sample")

    def remove_sample(self, channel_id):
        rc = lib.nclshim_server_remove_sample(self._require_open(), encode(channel_id))
        if rc != 0:
            raise NclinkError(rc, "remove_sample")

    def stop_all_samples(self):
        lib.nclshim_server_stop_all_samples(self._require_open())

    # ------------------------------------------------------------ HTTP -- #

    def start_http(self, port=9008, with_config=True):
        """起 HTTP 端点，返回 `HttpEndpoint`（用完 close；`server.close()` 也会收）。

        端点内容（全在库里）：

        * REST：`GET /api/schema`（OpenAPI 3.0 文档）、`GET /swagger-ui`、
          `POST /api/<工具>/<方法>`（等价于 methodCall）；
        * `with_config=True` 再挂配置端点：`GET /api/cfg/getSn`、`POST /api/cfg/init`、
          `GET|POST /api/cfg/getModel|setModel`、`getDriver|setDriver`、
          `GET /api/getMqttUrl` + `POST /api/setMqttUrl`、
          `GET|POST /api/method/getServerList|setServer`。

        `port=0` 用系统分配的随机端口（端口从 `HttpEndpoint.port` 读）。
        """
        handle = lib.nclshim_http_start(int(port), self._require_open(),
                                        1 if with_config else 0)
        if not handle:
            raise NclinkError(-5, "start_http", "HTTP 端口 %s 起不来（被占用？）" % port)
        endpoint = HttpEndpoint(self, handle, lib.nclshim_http_port(handle))
        self._http.append(endpoint)
        return endpoint

    # ------------------------------------------------------------ 事件 -- #

    def push_event(self, event_id, event, time_ms=None, message_id=None):
        """推一条事件到 `Event/<sn>`：`event` 形如 `{"key":..., "value":...}`。"""
        handle = self._require_open()
        body = _json_body(event)
        if time_ms is None and message_id is None:
            rc = lib.nclshim_server_push_event(handle, encode(event_id), body)
        else:
            rc = lib.nclshim_server_push_event_ex(handle, encode(event_id), body,
                                                  -1 if time_ms is None else int(time_ms),
                                                  encode(message_id))
        if rc != 0:
            raise NclinkError(rc, "push_event")

    # ------------------------------------------------------------ 收尾 -- #

    def close(self):
        """停采样、停 FTP、断开 MQTT、释放工具回调（可重复调用）。"""
        if self._handle is None:
            return
        for endpoint in self._http:          # 先收 HTTP：它的路由回调还挂在服务器上
            endpoint.close()
        self._http = []
        lib.nclshim_server_free(self._handle)
        self._handle = None
        self._tools = []
        self._handlers = {}
        self._publish_callback = None
        self._publish_host = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    def __str__(self):
        return "Server(%s)" % self._sn

    __repr__ = __str__

    # -------------------------------------------------------- 回调桥 -- #

    @staticmethod
    def _host_for(callback):
        """垫片要的 host 是两格 [函数指针, 用户数据]（用户数据这里不用）。"""
        slots = (ctypes.c_void_p * 2)()
        slots[0] = ctypes.cast(callback, ctypes.c_void_p)
        slots[1] = None
        return ctypes.cast(slots, ctypes.c_void_p)


def _parse_response(topic, text):
    """应答报文文本 -> Message / Sample / Event（复用 nclink.parse 的规则）。"""
    from . import parse as _parse

    if text is None:
        raise NclinkError(ERR, "dispatch", "没有应答")
    return _parse(topic, text)


def _response_topic(request_topic):
    """请求主题 -> 应答主题：Query/Request/<sn> -> Query/Response/<sn>。"""
    if request_topic is None:
        return None
    if request_topic.startswith("Ping/"):
        # 心跳的应答在 Pong/<sn>（Pong 只带一个 code 状态）
        return "Pong/" + request_topic[len("Ping/"):]
    return request_topic.replace("/Request/", "/Response/", 1)

class HttpEndpoint:
    """设备端的 HTTP / REST 端点（OpenAPI 文档 + Swagger UI + 工具端点 [+ 配置端点]）。

    由 `Server.start_http()` 创建，**关的时候要在 server.close() 之前**（它的路由回调
    还挂在服务器上）。想自己加 REST 端点就 `route()`。
    """

    def __init__(self, server, handle, port):
        self._server = server
        self._handle = handle
        self._port = int(port)
        self._routes = []          # ctypes 回调与它的 host 槽，close() 前不能被 GC

    @property
    def port(self):
        """实际绑定的端口（`start_http(0)` 时是系统给的随机端口）。"""
        return self._port

    @property
    def url(self):
        return "http://localhost:%d" % self._port

    @property
    def request_count(self):
        """已处理的请求数（诊断用）。"""
        if self._handle is None:
            return 0
        return int(lib.nclshim_http_request_count(self._handle))

    def set_cors(self, enabled=True):
        """Access-Control-Allow-Origin: *（默认开）。"""
        lib.nclshim_http_set_cors(self._require_open(), 1 if enabled else 0)

    def route(self, method, path, handler):
        """挂一条自己的路由。

        `method` 支持 `"*"`；`path` 以 `/api/` 开头时是前缀匹配（见
        `ncl_http_server_route` 的规则）。`handler(method, path, query, body)` 返回：

        * `None`：200，空报文；
        * `str`：200，text/plain（UTF-8）；
        * `dict` / `list` / 其它可 JSON 对象：200，application/json；
        * `(status, content_type, body)`：完全自己决定（body 为 None 只改状态码）。
        """

        def dispatch(user, method, path, query, body, out_status, out_content_type,
                     out_body):
            try:
                result = handler(decode(method), decode(path), decode(query),
                                 decode(body))
            except Exception as exc:                 # 不能穿回 C
                self._server.last_callback_error = exc
                out_status[0] = 500
                out_content_type[0] = strdup("text/plain; charset=utf-8")
                out_body[0] = strdup("handler failed: %s" % exc)
                return -1
            status, content_type, text = _normalise_reply(result)
            out_status[0] = status
            if content_type:
                out_content_type[0] = strdup(content_type)
            if text is not None:
                out_body[0] = strdup(text)
            return 0

        callback = ROUTE_CALLBACK(dispatch)
        slots = (ctypes.c_void_p * 2)()
        slots[0] = ctypes.cast(callback, ctypes.c_void_p)
        slots[1] = None
        host = ctypes.cast(slots, ctypes.c_void_p)
        rc = lib.nclshim_http_route(self._require_open(), encode(method),
                                    encode(path), host)
        if rc != 0:
            raise NclinkError(rc, "route", path)
        self._routes.append((callback, slots))

    def close(self):
        """停掉 HTTP 监听（可重复调用）。"""
        if self._handle is None:
            return
        lib.nclshim_http_free(self._handle)
        self._handle = None
        self._routes = []

    def _require_open(self):
        if self._handle is None:
            raise NclinkError(-13, "HttpEndpoint", "HTTP 端点已关闭")
        return self._handle

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    def __str__(self):
        return "HttpEndpoint(%s%s)" % (self.url, "" if self._handle else " closed")

    __repr__ = __str__


def _normalise_reply(result):
    """处理函数的返回值 -> (status, content_type, body_text)。"""
    if result is None:
        return 200, None, None
    if isinstance(result, tuple):
        status = int(result[0])
        content_type = result[1] if len(result) > 1 else "text/plain; charset=utf-8"
        body = result[2] if len(result) > 2 else None
        if body is None:
            return status, content_type, None
        return status, content_type, body if isinstance(body, str) else _json_stdlib.dumps(
            body, ensure_ascii=False)
    if isinstance(result, str):
        return 200, "text/plain; charset=utf-8", result
    return 200, "application/json; charset=utf-8", _json_stdlib.dumps(
        result, ensure_ascii=False)
