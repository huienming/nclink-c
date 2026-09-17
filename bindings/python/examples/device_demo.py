#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Python 设备端示例：这个 Python 进程就是一台机床。

    python examples/device_demo.py [broker] [设备SN] [秒数] [HTTP端口]
    # 默认：tcp://127.0.0.1:1883、V2PY0000001、一直运行到 Ctrl+C、HTTP 9008
    # 秒数写 0 = 一直跑；HTTP 端口写 0 = 用系统分配的随机端口

它注册工具方法、绑定模型里的路径、启动采样通道、每秒推一条事件；然后用仓库里
**任何一个客户端**都能读它，例如 C 的示例：

    build\\examples\\ncl_client_demo.exe tcp://127.0.0.1:1883 V2PY0000001 8

还起一个 REST 端点（OpenAPI 文档 + Swagger UI + 工具端点 + 配置端点），浏览器
打开 http://localhost:9008/swagger-ui 就能直接调它的工具方法。

模型（含采样通道）直接写在下面的 MODEL 里：换模型就是换这段 JSON。
"""

import json
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import nclink  # noqa: E402

MODEL = {
    "name": "Python 模拟机床",
    "id": "01",
    "type": "NC_LINK_ROOT",
    "devices": [{
        "id": "02",
        "type": "MACHINE",
        "name": "模拟机床",
        "version": "2.0",
        "configs": [{
            "name": "采样通道",
            "id": "py_channel",
            "type": "SAMPLE_CHANNEL",
            "sampleInterval": 1000,
            "uploadInterval": 1000,
            "ids": [{"id": "/STATUS"}, {"id": "/PART_COUNT"}, {"id": "/CONTROLLER/WARNNING"}],
        }],
        "dataItems": [
            {"name": "状态", "id": "030001", "type": "STATUS", "settable": False,
             "description": "1=运行"},
            {"name": "加工计件", "id": "030002", "type": "PART_COUNT", "settable": True,
             "description": "加工计件,单位:件"},
            {"name": "报警", "id": "030003", "type": "WARNNING", "source": "CONTROLLER"},
        ],
    }],
}


class Machine:
    """被工具方法读写的"机床状态"（真实设备里换成你的 PLC / 采集卡）。"""

    def __init__(self):
        self.status = 1
        self.part_count = 0
        self.warning = 0

    def get_status(self, params):
        return self.status

    def get_count(self, params):
        return self.part_count

    def set_count(self, params):
        self.part_count = int((params or {}).get("value", 0))
        return self.part_count

    def get_warning(self, params):
        return self.warning


def main(argv):
    broker = argv[1] if len(argv) > 1 else "tcp://127.0.0.1:1883"
    sn = argv[2] if len(argv) > 2 else "V2PY0000001"
    seconds = int(argv[3]) if len(argv) > 3 else 0
    http_port = int(argv[4]) if len(argv) > 4 else 9008

    machine = Machine()
    stop = {"flag": False}

    def on_signal(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_signal)

    nclink.log_init()
    device = nclink.Server(sn=sn, model=json.dumps(MODEL, ensure_ascii=False), broker=broker)
    uploads = 0
    events = 0
    try:
        device.register_tool(
            "plc",
            methods={"getStatus": None, "getCount": None, "setCount": None,
                     "getWarning": None},
            handlers={"getStatus": machine.get_status, "getCount": machine.get_count,
                      "setCount": machine.set_count, "getWarning": machine.get_warning},
            bindings=[("/STATUS", nclink.Operation.GET_VALUE, "getStatus"),
                      ("/PART_COUNT", nclink.Operation.GET_VALUE, "getCount"),
                      ("/PART_COUNT", nclink.Operation.SET_VALUE, "setCount"),
                      ("/CONTROLLER/WARNNING", nclink.Operation.GET_VALUE, "getWarning")])
        device.register_builtin_tool()          # addSample / removeSample
        device.register_file_tool()             # /CONTROLLER/FILE：文件通道
        try:
            device.start_ftp()                  # 设备自己的 FTP 端点（读 bin/ftp.txt）
        except nclink.NclinkError:
            print("提示：bin/ftp.txt 还没有，跳过设备侧 FTP 端点（客户端传文件不受影响）。")
        device.subscribe()                      # 订阅 6 个请求主题
        device.init_samples()                   # 启动模型里声明的采样通道

        # REST 端点：库自带 OpenAPI + swagger-ui + 工具端点 + 配置端点
        http = device.start_http(http_port, with_config=True)
        # 自己加一条路由（处理函数拿 method/path/query/body，返回 JSON/文本/元组）
        http.route("GET", "/api/hello",
                   lambda method, path, query, body: {"sn": sn, "parts": machine.part_count})
        print("HTTP: %s/api/schema（Swagger UI: %s/swagger-ui）" % (http.url, http.url))

        print("设备端已就绪：SN=%s broker=%s，工具 %d 个操作，采样通道 %d 个"
              % (sn, broker, device.operation_count, device.sample_count))
        print("（用仓库里的任意客户端读它，例如 build\\examples\\ncl_client_demo.exe）")

        deadline = time.time() + seconds if seconds > 0 else None
        while not stop["flag"] and (deadline is None or time.time() < deadline):
            time.sleep(1.0)
            machine.part_count += 1                       # 模拟产量累加
            machine.status = 1
            machine.warning = 0
            device.push_event("010307", {"key": "PART_COUNT",
                                         "value": machine.part_count,
                                         "oldValue": machine.part_count - 1})
            if machine.part_count % 5 == 0:
                print("已上报采样 %d 次，事件 %d 条（计件 %d）"
                      % (device.sample_upload_count, device.event_count,
                         machine.part_count))
    finally:
        uploads = device.sample_upload_count    # 计数要在关掉之前读
        events = device.event_count
        device.close()                          # 端点也一起收（device.close 里先收 HTTP）
        nclink.log_shutdown()
    print("设备端已退出：采样上报 %d 次，事件 %d 条" % (uploads, events))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nclink.NclinkError as error:
        print("NC-Link error: %s" % error)
        sys.exit(1)
