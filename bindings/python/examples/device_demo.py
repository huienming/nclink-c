#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Python 设备端示例：这个 Python 进程就是一台机床。

    python examples/device_demo.py [broker] [设备SN] [秒数]
    # 默认：tcp://127.0.0.1:1883、V2PY0000001、一直运行到 Ctrl+C

它注册工具方法、绑定模型里的路径、启动采样通道、每秒推一条事件；然后用仓库里
**任何一个客户端**都能读它，例如 C 的示例：

    build\\examples\\ncl_client_demo.exe tcp://127.0.0.1:1883 V2PY0000001 8

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

    machine = Machine()
    stop = {"flag": False}

    def on_signal(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_signal)

    nclink.log_init()
    device = nclink.Server(sn=sn, model=json.dumps(MODEL, ensure_ascii=False), broker=broker)
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
        device.subscribe()                      # 订阅 6 个请求主题
        device.init_samples()                   # 启动模型里声明的采样通道
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
        device.close()
        nclink.log_shutdown()
    print("设备端已退出：采样上报 %d 次，事件 %d 条"
          % (device.sample_upload_count, device.event_count))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nclink.NclinkError as error:
        print("NC-Link error: %s" % error)
        sys.exit(1)