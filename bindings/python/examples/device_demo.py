#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""设备端示例：这个 Python 进程就是一台机床。

    python examples/device_demo.py [broker] [sn] [seconds] [http-port]

      broker    tcp://… / ssl://…；写 "-" = 离线（不接 MQTT，出站报文打控制台）；
                省略 = 读 <root>/conf/mqtt.cfg（没有就先写一份默认的）
      sn        省略或 "-" = <root>/bin/sn.txt（没有就生成 "V2" + 9 位十六进制）
      seconds   0 或省略 = 一直运行到 Ctrl+C
      http-port 省略 = 9008；0 = 系统分配的随机端口
      安装根目录：环境变量 NCL_DEVICE_ROOT（省略 = 当前目录）

**五个语言的设备端示例是同一台设备**：同一个模型（仓库里的
examples/device_model.json，首次启动拷进 <root>/conf/model/nclink.json）、同一批
工具方法与 <operation>#<path> 绑定、同样的两个采样通道与事件节拍（100 ms 一跳，
每秒一条 PART_COUNT 事件）。模型里每个轴都有一个功率（/AXIS@<轴>/POWER@1）与
三个加速度（/AXIS@<轴>/ACCELERATION@X|Y|Z）—— 振动信号在三个方向上的分量。

用仓库里任意一个客户端都能读它，例如 C 的示例：

    build\\examples\\ncl_client_demo.exe tcp://127.0.0.1:1883 <SN> 8
"""

import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import nclink  # noqa: E402

AXES = ("X", "Y", "Z", "C", "S")
DIRS = ("X", "Y", "Z")
DEFAULT_BROKER = "tcp://127.0.0.1:1883"

# 工具方法名与 "<operation>#<路径>" 绑定（C / C# / Java / Go 的示例一模一样）。
METHODS = [
    "getValue",
    "setValue",
    "getCount",
    "getWarning",
    "getProgram",
    "getToolNumber",
    "getFeedOverride",
    "getMachiningMode",
    "getSpeedS",
    "getPowerX",
    "getPowerY",
    "getPowerZ",
    "getPowerC",
    "getPowerS",
    "getAccelerationXX",
    "getAccelerationXY",
    "getAccelerationXZ",
    "getAccelerationYX",
    "getAccelerationYY",
    "getAccelerationYZ",
    "getAccelerationZX",
    "getAccelerationZY",
    "getAccelerationZZ",
    "getAccelerationCX",
    "getAccelerationCY",
    "getAccelerationCZ",
    "getAccelerationSX",
    "getAccelerationSY",
    "getAccelerationSZ",
]
BINDINGS = [
    ("/STATUS", nclink.Operation.GET_VALUE, "getValue"),
    ("/STATUS", nclink.Operation.SET_VALUE, "setValue"),
    ("/PART_COUNT", nclink.Operation.GET_VALUE, "getCount"),
    ("/CONTROLLER/WARNING", nclink.Operation.GET_VALUE, "getWarning"),
    ("/CONTROLLER/PROGRAM", nclink.Operation.GET_VALUE, "getProgram"),
    ("/CONTROLLER/TOOL_NUMBER", nclink.Operation.GET_VALUE, "getToolNumber"),
    ("/FEED_OVERRIDE", nclink.Operation.GET_VALUE, "getFeedOverride"),
    ("/MACHINING_MODE", nclink.Operation.GET_VALUE, "getMachiningMode"),
    ("/AXIS@S/SPEED", nclink.Operation.GET_VALUE, "getSpeedS"),
    ("/AXIS@X/POWER@1", nclink.Operation.GET_VALUE, "getPowerX"),
    ("/AXIS@Y/POWER@1", nclink.Operation.GET_VALUE, "getPowerY"),
    ("/AXIS@Z/POWER@1", nclink.Operation.GET_VALUE, "getPowerZ"),
    ("/AXIS@C/POWER@1", nclink.Operation.GET_VALUE, "getPowerC"),
    ("/AXIS@S/POWER@1", nclink.Operation.GET_VALUE, "getPowerS"),
    ("/AXIS@X/ACCELERATION@X", nclink.Operation.GET_VALUE, "getAccelerationXX"),
    ("/AXIS@X/ACCELERATION@Y", nclink.Operation.GET_VALUE, "getAccelerationXY"),
    ("/AXIS@X/ACCELERATION@Z", nclink.Operation.GET_VALUE, "getAccelerationXZ"),
    ("/AXIS@Y/ACCELERATION@X", nclink.Operation.GET_VALUE, "getAccelerationYX"),
    ("/AXIS@Y/ACCELERATION@Y", nclink.Operation.GET_VALUE, "getAccelerationYY"),
    ("/AXIS@Y/ACCELERATION@Z", nclink.Operation.GET_VALUE, "getAccelerationYZ"),
    ("/AXIS@Z/ACCELERATION@X", nclink.Operation.GET_VALUE, "getAccelerationZX"),
    ("/AXIS@Z/ACCELERATION@Y", nclink.Operation.GET_VALUE, "getAccelerationZY"),
    ("/AXIS@Z/ACCELERATION@Z", nclink.Operation.GET_VALUE, "getAccelerationZZ"),
    ("/AXIS@C/ACCELERATION@X", nclink.Operation.GET_VALUE, "getAccelerationCX"),
    ("/AXIS@C/ACCELERATION@Y", nclink.Operation.GET_VALUE, "getAccelerationCY"),
    ("/AXIS@C/ACCELERATION@Z", nclink.Operation.GET_VALUE, "getAccelerationCZ"),
    ("/AXIS@S/ACCELERATION@X", nclink.Operation.GET_VALUE, "getAccelerationSX"),
    ("/AXIS@S/ACCELERATION@Y", nclink.Operation.GET_VALUE, "getAccelerationSY"),
    ("/AXIS@S/ACCELERATION@Z", nclink.Operation.GET_VALUE, "getAccelerationSZ"),
]


class Machine:
    """被工具方法读写的"机床状态"（真机里换成你的 PLC / 采集卡）。"""

    def __init__(self):
        self.status = 1
        self.part_count = 0
        self.warning = 0
        self.program = 1001
        self.tool_number = 1
        self.feed_override = 100
        self.spindle_speed = 3600
        self.mode = 2
        self.power_tick = {axis: 0 for axis in AXES}
        self.vibration_tick = {(axis, d): 0 for axis in AXES for d in DIRS}

    def power(self, axis):
        """功率（W）：每轴一个基值 + 0~300 W 的缓升，25 格一个锯齿。"""
        slot = AXES.index(axis)
        tick = self.power_tick[axis]
        self.power_tick[axis] += 1
        return 800.0 + slot * 250.0 + ((tick + slot * 7) % 25) * 12.5

    def vibration(self, axis, direction):
        """振动：一次查询给 4 个 0.25 ms 子采样（1 ms 槽位里的 4 kHz 波形）。

        返回数组 —— 库会把它摊平成一列；X/Y/Z 三个方向的相位各不相同。
        """
        slot = AXES.index(axis) * len(DIRS) + DIRS.index(direction)
        step = self.vibration_tick[(axis, direction)]
        self.vibration_tick[(axis, direction)] += 4
        return [(((step + k + slot * 3) % 16) - 8) * 0.125 for k in range(4)]

    def handle(self, method, params):
        if method == "setValue":
            self.status = int((params or {}).get("value", 0))
            print("STATUS 被设置为 %d" % self.status)
            return True
        table = {
            "getValue": lambda: self.status,
            "getCount": lambda: self.part_count,
            "getWarning": lambda: self.warning,
            "getProgram": lambda: self.program,
            "getToolNumber": lambda: self.tool_number,
            "getFeedOverride": lambda: self.feed_override,
            "getMachiningMode": lambda: self.mode,
            "getSpeedS": lambda: self.spindle_speed,
        }
        for name in METHODS:
            if name.startswith("getPower"):
                table[name] = (lambda a=name[len("getPower"):]: self.power(a))
            elif name.startswith("getAcceleration"):
                axis = name[len("getAcceleration")]
                direction = name[len("getAcceleration") + 1]
                table[name] = (lambda a=axis, d=direction: self.vibration(a, d))
        if method not in table:
            raise ValueError("没有方法 " + method)
        return table[method]()


def read_mqtt_cfg(root):
    path = os.path.join(root, "conf", "mqtt.cfg")
    try:
        with open(path, "r", encoding="utf-8") as fp:
            for line in fp:
                if line.strip().startswith("url="):
                    return line.strip()[4:].strip() or DEFAULT_BROKER
    except OSError:
        pass
    return DEFAULT_BROKER


def bootstrap(root):
    for sub in ("conf", "bin", "log"):
        os.makedirs(os.path.join(root, sub), exist_ok=True)
    cfg = os.path.join(root, "conf", "mqtt.cfg")
    if not os.path.exists(cfg):
        with open(cfg, "w", encoding="utf-8") as fp:
            fp.write("url=%s\r\nusername=\r\npassword=\r\n" % DEFAULT_BROKER)
        print("首次启动：写入 MQTT 配置（%s）" % cfg)


def read_sn(root):
    """<root>/bin/sn.txt；没有就按库的规则生成（"V2" + 9 位十六进制）。"""
    path = os.path.join(root, "bin", "sn.txt")
    try:
        with open(path, "r", encoding="utf-8") as fp:
            text = fp.read().strip()
        if text:
            return text
    except OSError:
        pass
    import random
    digits = "0123456789ABCDEF"
    while True:
        sn = "V2" + "".join(random.choice(digits) for _ in range(9))
        if any(c in "ABCDEF" for c in sn):
            break
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(sn)
    print("首次启动：生成 SN（%s）" % path)
    return sn


def load_model(root):
    """<root>/conf/model/nclink.json 优先；没有就用仓库里的共享模型自举。"""
    installed = os.path.join(root, "conf", "model", "nclink.json")
    if os.path.exists(installed):
        with open(installed, "r", encoding="utf-8") as fp:
            return fp.read()
    text = nclink.device_model()
    os.makedirs(os.path.dirname(installed), exist_ok=True)
    with open(installed, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)
    print("首次启动：写入设备模型（%s，编译进垫片的那份）" % installed)
    return text


def main(argv):
    broker_arg = argv[1] if len(argv) > 1 else ""
    sn_arg = argv[2] if len(argv) > 2 else ""
    seconds = int(argv[3]) if len(argv) > 3 else 0
    http_port = int(argv[4]) if len(argv) > 4 else 9008
    root = os.environ.get("NCL_DEVICE_ROOT") or "."
    offline = broker_arg == "-"

    bootstrap(root)
    nclink.set_root(root)
    nclink.log_init()

    sn = sn_arg if sn_arg not in ("", "-") else read_sn(root)
    broker = None if offline else (broker_arg or read_mqtt_cfg(root))
    model_text = load_model(root)

    machine = Machine()
    stop = {"flag": False}

    def on_signal(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_signal)

    def console_sink(topic, payload):
        print("out  %s  %s" % (topic.decode("utf-8", "replace"),
                                 payload.decode("utf-8", "replace")))

    device = nclink.Server(sn=sn, model=model_text, broker=broker,
                           publish=console_sink if offline else None)
    try:
        device.register_tool(
            "plc",
            methods={name: None for name in METHODS},
            handlers={name: (lambda m=name: machine.handle(m, None))
                      for name in METHODS},
            bindings=BINDINGS)
        device.register_builtin_tool()
        device.register_file_tool()
        try:
            device.start_ftp()
        except nclink.NclinkError:
            pass
        if not offline:
            device.subscribe()
        device.init_samples()

        http = device.start_http(http_port, with_config=True)
        http.route("GET", "/api/hello", lambda method, path, query, body:
                   {"sn": sn, "status": machine.status, "parts": machine.part_count})

        print("设备 SN: %s" % sn)
        print("MQTT: %s" % (broker if not offline else "离线模式（出站报文打到控制台）"))
        print("HTTP: %s/swagger-ui（自定义路由 /api/hello）" % http.url)
        print("工具 %d 个操作，采样通道 %d 个" % (device.operation_count,
                                                   device.sample_count))
        print("运行 %s" % (("%d 秒（Ctrl+C 可随时退出）" % seconds) if seconds > 0
                           else "直到 Ctrl+C"))

        deadline = time.time() + seconds if seconds > 0 else None
        i = 0
        uploads = 0
        events = 0
        while not stop["flag"] and (deadline is None or time.time() < deadline):
            time.sleep(0.1)
            i += 1
            machine.part_count += 1
            machine.feed_override = 60 + (i % 7) * 10
            machine.spindle_speed = 3000 + (i % 5) * 300
            if i % 20 == 0:
                machine.program += 1
                machine.tool_number = 1 + machine.program % 8
                machine.mode = 1 if machine.program % 2 == 0 else 2
            if i % 10 == 0:
                device.push_event("010307", {
                    "key": "PART_COUNT", "value": machine.part_count,
                    "oldValue": machine.part_count - 1})
                uploads = device.sample_upload_count
                events = device.event_count
                print("事件 PART_COUNT=%d；采样上报 %d 次，状态 %d，刀号 %d"
                      % (machine.part_count, uploads, machine.status,
                         machine.tool_number))
    finally:
        uploads = device.sample_upload_count
        events = device.event_count
        device.close()
        nclink.log_shutdown()
    print("设备端退出统计：采样上报 %d 次，事件 %d 条" % (uploads, events))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nclink.NclinkError as error:
        print("NC-Link error: %s" % error)
        sys.exit(1)
