#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""真 FOCAS2 假机床：字节按"已核过的证据"铺，官方 SDK 与 nclink-c 都能连。

和 `focas_sdk_mock.py` 的区别：那一台是"载荷随便铺、看 SDK 怎么解"的**取证**工具；
这一台是**按核出来的口径发正确的字节**，用来把 client 端到端跑起来（STATUS / 件数 /
程序号 / 坐标 / 指令位置 / 进给 / 主轴 / 报警 / 时钟 / 块数）。

    python focas_machine.py 8193 --pos 12.345,67.89,0 --feed 500 --spindle 3000 \
        --count 952 --prog O1234 --alarm 0 --status free --mode auto --srv-delay 1.234

**每个字段的字节是从哪来的**（这是这个工具的意义，也是它的边界）：

1. 帧与块结构  —— 参考实现反汇编 + 假机床实测（01 册 §2.2/§2.3）：`a0a0a0a0` +
   大端 `type(2)/func(1)/dir(1)/len(2)`；`func 0x21` 的应答 = `块个数(2)` + N 个
   "`本块长(2)` + 4 字节 + `返回码(2)` + 4 字节 + `载荷长(2)` + 载荷"（块长 ≥ 34）。
2. 每个 item 的 Cb 码（`c`/`d`/`e`）—— 官方 SDK 自己发的请求帧（假机床抓的，§2.4）。
3. 载荷里字段的位置 —— 官方库把应答拷进它自己的结构体，读出来就知道谁在哪儿
   （本文件里的 `STATINFO` 布局就是这么钉的：块 1 → dummy、块 2 → aut、
   块 0 载荷 → manual/run/edit/…）。
4. 数值本身的形状（`POSELM` 12 字节、`ODBAXIS` 一族 8 字节、`ACTF` 每轴 float、
   `RDPRG@2/@6`…）—— NCGuide HSSB 实测（§2.5）。

**没被任何一条证据覆盖的 item，这里一律不做**（回一个"无此块"的错块），而不是编字节。
"""
import argparse
import json
import socket
import struct
import sys
import threading
import time
import urllib.request

MAGIC = b"\xa0\xa0\xa0\xa0"
TYPE_NORMAL = 0x0001
FUNC_HELLO = 0x01     # 握手第一帧（体 2 字节），应答 = 16 + 8n 字节
FUNC_CMD = 0x21       # 数据请求：体 = 块个数 + N×28 字节 Cb
FUNC_BYE = 0x02

HEADER = 10
CB_SIZE = 28
AXIS_NAMES = "XYZAC"

# 坐标那条请求里的"框/轴信息"块：`0x0e` 同时被当能力块用（d = e = 0x26f0）和
# 坐标那条的第 7 个块用（d = e = 0xc2b = 3115），所以按**码**兜底，不按 d/e 匹配。
FILLER_CODES = (0x19, 0x89, 0x0e, 0x88)

# 单条调用的 d（= Cb 的 arg0）与 `cnc_rdposition` 那条**不是一套编号**：
#   cnc_absolute 4 / cnc_machine 1 / cnc_relative 6 / cnc_distance 7 /
#   cnc_srvdelay 9 / cnc_accdecdly 10 / cnc_skip 8      ← 官方库反汇编（01 册 §2.5.1）
#   cnc_rdposition 一条 4 个 0x26：d = 0 绝对 / 1 机械 / 2 相对 / 3 剩余
POSITION_D = {0: "abs", 4: "abs", 1: "machine", 2: "rel", 6: "rel",
              3: "dist", 7: "dist"}


def hexdump(data):
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("%04x  %-47s  %s" % (
            i,
            " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return "\n".join(out)


def be16(data, off):
    return int.from_bytes(data[off:off + 2], "big")


def be32(data, off):
    return int.from_bytes(data[off:off + 4], "big")


def cbs_of(body):
    """请求体里的 Cb：[(code, d, e)]。"""
    out = []
    if len(body) < 2:
        return out
    count = be16(body, 0)
    for i in range(count):
        off = 2 + i * CB_SIZE
        if len(body) < off + CB_SIZE:
            break
        out.append((be16(body, off + 6), be32(body, off + 8),
                    be32(body, off + 12)))
    return out


class Machine:
    """一台假机床的"当前状态"——命令行给什么就发什么。"""

    def __init__(self, args):
        self.axes = [float(v) for v in args.pos.split(",")]
        while len(self.axes) < len(AXIS_NAMES):
            self.axes.append(0.0)
        self.axis_count = int(args.axes_count)
        self.max_axes = int(args.max_axes)
        self.cap_hex = args.cap_hex
        self.srv_shape = args.srv_shape
        self.hello_records = int(args.hello_records)
        self.hello_hex = args.hello_hex
        self.rec18 = bytes.fromhex(args.rec18) if args.rec18 else b"\x00" * 16
        self.rec_a = int(args.rec_a)
        self.rec_c = int(args.rec_c)
        self.rec_d = int(args.rec_d)
        self.machine = [float(v) for v in args.machine.split(",")] \
            if args.machine else list(self.axes)
        while len(self.machine) < len(AXIS_NAMES):
            self.machine.append(0.0)
        self.feed = float(args.feed)
        self.spindle = float(args.spindle)
        self.srv_delay = float(args.srv_delay)
        self.count = int(args.count)
        self.prog = args.prog
        self.main_prog = args.main_prog
        self.line = int(args.line)
        self.alarm = int(args.alarm)
        self.status = args.status
        self.mode = args.mode
        self.emergency = args.emergency == "on"
        self.blkcount = int(args.blkcount)
        self.tool_groups = int(args.tool_groups)
        self.run_prog = int(args.run_prog)
        self.main_prog = int(args.main_prog)
        self.timers = {
            0: int(args.power_minutes),
            1: int(args.run_minutes),
            2: int(args.cutting_minutes),
            3: 0,
            4: 0,
        }

    # ---- 每个 item 的载荷（值 = data / 10^dec 的那一族，dec 取 3） -------------

    def poselm(self, values):
        """POSELM：int32 data + dec(2) + unit(2) + disp(2) + 轴名(1) + 后缀(1)。"""
        out = bytearray()
        for i in range(self.axis_count):
            name = AXIS_NAMES[i] if i < len(AXIS_NAMES) else "?"
            data = int(round(values[i] * 1000.0))
            out += struct.pack(">i", data)
            out += struct.pack(">HHH", 3, 0, 1)
            out += bytes([ord(name), 0])
        return bytes(out)

    def odbaxis(self, value):
        """ODBAXIS 一族（伺服延迟量）。三种候选形状，用 `--srv-shape` 选——让官方 SDK 把
        `ODBAXIS.data[0]` 填出来，填得出来的那个就是线上形状：

        `rec8`  每轴 8 字节记录（值在第 0 个 int32，后 4 字节 dec/unit）← 反汇编推的
        `bare4` 每轴 4 字节的纯 int32 数组
        `hdr4`  4 字节头（dummy/type）+ 每轴 4 字节
        """
        data = int(round(value * 1000.0))
        if self.srv_shape == "bare4":
            return b"".join(struct.pack(">i", data)
                            for _ in range(self.axis_count))
        if self.srv_shape == "hdr4":
            return struct.pack(">HH", 0, 0) + b"".join(
                struct.pack(">i", data) for _ in range(self.axis_count))
        out = bytearray()
        for _ in range(self.axis_count):
            out += struct.pack(">i", data)
            out += struct.pack(">HH", 3, 0)
        return bytes(out)

    def odbsys(self):
        """能力块 / `cnc_sysinfo` 的 ODBSYS：addinfo、**max_axis**、类型、系列、版本…

        布局照着 NCGuide 上 `cnc_sysinfo` 的实测字节铺（01 册 §2.5）：
        `42 06 | 20 00 | ' ' | '0' | ' ' | 'M' | "D4G2-49.0" | '0' | '3'`，
        其中 `[2..4)` = 最大轴数。
        """
        if self.cap_hex:
            # 直接用给的那串（试"驱动到底在哪一格读轴数"时最省事）
            return bytes.fromhex(self.cap_hex).ljust(40, b"\x00")
        out = bytearray(struct.pack(">HH", 0x4206, self.max_axes))
        out += b" 0 M"                  # cnc_type / mt_type / series
        out += b"D4G2-49.0"             # version
        out += b"03\x00"                # path / 补齐
        return bytes(out)

    def statinfo(self):
        """ODBST：块 1 = dummy、块 2 = aut、块 0 载荷 = manual…oper（9 个 u16）。

        布局是拿"斜坡载荷 + 官方 SDK 填结构体"钉出来的（见 tools/site-probe/README.md）：
        结构体偏移 0 收块 1、偏移 2 收块 2、偏移 4 起收块 0 的载荷。
        """
        manual = 1 if self.mode == "manual" else 0
        aut = 1 if self.mode == "auto" else 0
        run = 1 if self.status == "running" else 0
        # 我们 client 的三态是从 RUN + EMERGENCY 推的（表 6/表 8），所以 --status
        # holding 也置 EMERGENCY 位 —— 否则自检那行只会看到 "free"。
        hold = 1 if self.status == "holding" else 0
        emergency = hold or (1 if self.emergency == "on" else 0)
        fields = [manual, run, 0, 0, 0, emergency,
                  self.alarm, 0, 0, 0, 0]   # manual,run,edit,motion,mstb,emergency,…
        return {
            "statinfo": [b"".join(struct.pack(">H", v) for v in fields),
                         struct.pack(">H", 0),
                         struct.pack(">H", aut)],
        }

    def items(self):
        table = {}
        # 0x26 那一族：两种编号都铺上（单条调用 d = 4/1/6/7/9，rdposition 那条 d = 0..3）
        for d, which in POSITION_D.items():
            values = {"abs": self.axes, "machine": self.machine}.get(which)
            if values is None:
                values = [0.0] * len(AXIS_NAMES)
            table[(0x26, d)] = self.poselm(values)
        table[(0x26, 9)] = self.odbaxis(self.srv_delay)        # 跟踪误差
        # 注意：`0x19` = 25，和 cnc_statinfo 的第一个 Cb 是同一个码 —— 靠"Cb 组合"
        # 区分（statinfo 只有 25/225/152 三个，坐标那条 9 个）。见 reply_for_request()。
        table[(0x19, 0)] = b"\x00" * 4                         # 坐标那条两头的框
        table[(0x89, 0xffffffff)] = b"\x00" * 4
        # 能力块（握手的第 3 条，`system_info_v1`）：载荷就是 ODBSYS —— 官方库从这里
        # 读"这台机床几根轴"（NCGuide 上 cnc_sysinfo 显示 [2..4) = 0x0020 = 32 轴）。
        table[(0x0e, 0x26f0)] = self.odbsys()
        table[(0x88, 0)] = b"\x00" * 4
        table[(0x24, 0)] = b"".join(struct.pack(">f", self.feed)
                                    for _ in AXIS_NAMES)       # ACTF 进给
        table[(0x25, 0)] = b"".join(struct.pack(">f", self.spindle)
                                    for _ in range(4))         # ACTS 主轴
        table[(0x8b, 0)] = self.scalar(self.count)             # RDCOUNT 件数
        table[(0x8b, 1)] = struct.pack(">i", 0)                # RDLIFE（形状待核）
        prg = bytearray(12)
        prg[2:4] = struct.pack(">H", self.run_prog)            # RDPRG：@2 运行
        prg[6:8] = struct.pack(">H", self.main_prog)           #        @6 主
        table[(0x1c, 0)] = bytes(prg)
        table[(0x1d, 0)] = struct.pack(">I", self.line)        # RDSEQ
        table[(0x1a, 0)] = self.scalar(self.alarm)             # RDALM 报警状态位
        table[(0x4a, 0)] = self.scalar(self.tool_groups)       # RDNGROUP
        for kind, minutes in self.timers.items():
            table[(0x120, kind)] = self.scalar(minutes)
        table[(0xfc, 0)] = self.prog.encode()[:36].ljust(36, b"\x00")
        table[(0x35, 0)] = self.scalar(self.blkcount)          # RDBLKCOUNT
        table[(0x18, 0)] = self.rec18                          # 握手记录详情
        return table

    @staticmethod
    def scalar(value):
        """标量类 item 的载荷：值在前 4 字节，后面补零到 16 —— 官方 SDK 对太短的块
        会挑（`cnc_rdblkcount` 给 4 字节就回 rc = 6）。"""
        return struct.pack(">i", int(value)) + b"\x00" * 12

    def payload(self, code, d, e):
        table = self._table or {}
        key = (code, d)
        if key in table:
            return table[key]
        key = (code, e)
        if key in table:
            return table[key]
        if code in FILLER_CODES:
            return b"\x00" * 4
        return None

    _table = None

    def refresh(self):
        self._table = self.items()

    # ---- 从 ProtoForge 的 REST 拉点值（--protoforge） -------------------------

    def apply_points(self, points):
        """把 ProtoForge 那几个点映射到这台假机床的状态。

        映射按 `fanuc_focas_cnc` 模板的点名写（x_abs/y_abs/z_abs、feed_rate、
        spindle_speed、run_status、tool_number）；没有对应点的（件数、程序号、
        跟踪误差…）保持命令行给的值。`run_status` 的取值口径按它模板里的
        `free/running/holding`（0/1/2）猜的，对不上就改这一处。
        """
        def num(name):
            value = points.get(name)
            try:
                return float(value)
            except (TypeError, ValueError):
                return None

        for i, key in enumerate(("x_abs", "y_abs", "z_abs")):
            value = num(key)
            if value is not None:
                self.axes[i] = value
        value = num("feed_rate")
        if value is not None:
            self.feed = value
        value = num("spindle_speed")
        if value is not None:
            self.spindle = value
        value = num("tool_number")
        if value is not None:
            self.tool_groups = int(value)
        value = num("run_status")
        if value is not None:
            self.status = {0.0: "free", 1.0: "running", 2.0: "holding"}.get(
                value, self.status)

    def reply_for_request(self, wanted):
        """按 Cb **组合**分派：cnc_statinfo 的三个码（25/225/152）和坐标那条的 9 个
        块共用 `0x19`，只有整条请求的形状能区分。"""
        codes = [c for c, _, _ in wanted]
        if codes == [25, 225, 152]:
            return self.statinfo()["statinfo"]
        out = []
        for code, d, e in wanted:
            out.append(self.payload(code, d, e))
        return out


def block(payload):
    """一个应答块：本块长(2) + ecode(2) + 4 字节 + 返回码(2) + 4 字节 + 载荷长(2)。"""
    size = 16 + len(payload)
    out = bytearray(size)
    out[0:2] = struct.pack(">H", size)
    out[8:10] = struct.pack(">H", 0)          # 返回码 0 = OK
    out[14:16] = struct.pack(">H", len(payload))
    out[16:16 + len(payload)] = payload
    return bytes(out)


def error_block(code):
    """认不出来的 Cb：返回码非 0（SDK 会抛，client 会回 NCL_FOCAS_ERR_RB_CODE）。"""
    out = bytearray(16)
    out[0:2] = struct.pack(">H", 16)
    out[8:10] = struct.pack(">H", 1)
    out[14:16] = struct.pack(">H", 0)
    return bytes(out)


def frame(func, body):
    return (MAGIC + struct.pack(">HBBH", TYPE_NORMAL, func, 2, len(body)) +
            body)


def handle(conn, machine, verbose):
    buf = b""
    while True:
        try:
            chunk = conn.recv(4096)
        except OSError:
            return
        if not chunk:
            return
        buf += chunk
        while len(buf) >= HEADER:
            total = HEADER + be16(buf, 8)
            if len(buf) < total:
                break
            data, buf = buf[:total], buf[total:]
            func = data[6]
            body = data[HEADER:]
            if verbose:
                print("== req func=0x%02x dir=%d body=%d" %
                      (func, data[7], len(body)), flush=True)
                print(hexdump(data), flush=True)
                if func == FUNC_CMD:
                    for code, d, e in cbs_of(body):
                        print("   Cb code=0x%02x (%d) d=%d e=%d" %
                              (code, code, d, e), flush=True)
            if func == FUNC_HELLO:
                # 体 = 16 + 8n；§2.3 的判据 5 要求长度正好这么多。这 n 条记录就是
                # 驱动眼里的"受控轴"，轴数（ODBAXIS 的长度规则 4 + 4×轴数）由它来。
                n = machine.hello_records
                hello = bytearray(16 + 8 * n)
                if machine.hello_hex:
                    hello[0:16] = bytes.fromhex(machine.hello_hex)
                hello[8:10] = struct.pack(">H", n)
                for i in range(n):
                    # 每条记录 4 个 BE16（§2.3）：A / B / C / D。A 当"记录类型"试值
                    # （--rec-a），B 当序号。
                    struct.pack_into(">HHHH", hello, 16 + 8 * i,
                                     machine.rec_a, i + 1,
                                     machine.rec_c, machine.rec_d)
                reply = frame(func, bytes(hello))
            elif func == FUNC_CMD:
                machine.refresh()
                wanted = cbs_of(body)
                if not wanted:
                    wanted = [(0x0e, 0x26f0, 0x26f0)]
                payloads = machine.reply_for_request(wanted)
                out = bytearray(struct.pack(">H", len(wanted)))
                for (code, d, e), payload in zip(wanted, payloads):
                    if payload is None:
                        if verbose:
                            print("   (no handler: code=0x%02x d=%d e=%d)" %
                                  (code, d, e), flush=True)
                        out += error_block(code)
                    else:
                        out += block(payload)
                reply = frame(func, bytes(out))
            else:
                return  # bye：SDK 到这里就把连接关了
            if verbose:
                print("--- rsp %d bytes: %s" % (len(reply), reply[:48].hex()),
                      flush=True)
            try:
                conn.sendall(reply)
            except OSError:
                return


def serve(port, machine, verbose):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(4)
    print("fake FOCAS2 machine on 0.0.0.0:%d  (Ctrl-C 退出)" % port, flush=True)
    while True:
        conn, peer = listener.accept()
        if verbose:
            print("== connect from %s" % (peer,), flush=True)
        threading.Thread(target=handle, args=(conn, machine, verbose),
                         daemon=True).start()


def poll_protoforge(machine, base, device, token, interval_ms, verbose):
    """每几毫秒去 ProtoForge 拉一趟点值；拉不到就留着上一次的（并打一行）。"""
    url = "%s/api/v1/devices/%s" % (base.rstrip("/"), device)
    headers = {"Authorization": "Bearer " + token} if token else {}
    while True:
        try:
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, timeout=3) as response:
                data = json.loads(response.read().decode("utf-8"))
            points = {p.get("name"): p.get("value")
                      for p in data.get("points", [])}
            machine.apply_points(points)
            if verbose:
                keep = ("x_abs", "y_abs", "z_abs", "feed_rate",
                        "spindle_speed", "run_status")
                print("[protoforge] %s" % ", ".join(
                    "%s=%s" % (k, points[k]) for k in sorted(points)
                    if k in keep), flush=True)
        except Exception as exc:  # 网络/鉴权/JSON：报一次，慢一点再试
            print("[protoforge] 拉点失败：%s（保持上一次的值）" % exc, flush=True)
            time.sleep(interval_ms / 1000.0 * 4)
            continue
        time.sleep(interval_ms / 1000.0)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", type=int, default=8193)
    ap.add_argument("--pos", default="12.345,67.89,0,0,0",
                    help="绝对位置 mm，逗号分隔（默认 12.345,67.89,0,0,0）")
    ap.add_argument("--machine", default="",
                    help="机械坐标（默认跟绝对位置一样）")
    ap.add_argument("--feed", default="500", help="每轴进给速度 F（mm/min）")
    ap.add_argument("--spindle", default="3000", help="主轴转速 S（rpm）")
    ap.add_argument("--srv-delay", default="0",
                    help="伺服延迟量 = 跟踪误差（mm；给负数就是实际 > 指令）")
    ap.add_argument("--srv-shape", default="rec8",
                    choices=["rec8", "bare4", "hdr4"],
                    help="伺服延迟量那条的载荷形状（实验用，默认 rec8）")
    ap.add_argument("--axes-count", default="5", type=int,
                    help="应答里报几根轴（默认 5）")
    ap.add_argument("--max-axes", default="32", type=int,
                    help="能力块 ODBSYS 里的最大轴数（驱动用它算 ODBAXIS 长度；默认 32）")
    ap.add_argument("--cap-hex", default="",
                    help="能力块（Cb 0x0e d=e=0x26f0）载荷的 hex，给了就用它")
    ap.add_argument("--hello-records", default="0", type=int,
                    help="握手 func 01 报几条记录（驱动把这当受控轴数；默认 0）")
    ap.add_argument("--rec18", default="",
                    help="握手记录详情（Cb 码 0x18）的载荷 hex，默认 16 个 0")
    ap.add_argument("--rec-a", default="1", type=int, help="握手记录的 A 字段（类型）")
    ap.add_argument("--rec-c", default="0", type=int, help="握手记录的 C 字段")
    ap.add_argument("--rec-d", default="0", type=int, help="握手记录的 D 字段")
    ap.add_argument("--hello-hex", default="",
                    help="握手 func 01 应答头 16 字节的 hex（试「轴数在哪一格」用）")
    ap.add_argument("--count", default="952", help="件数")
    ap.add_argument("--prog", default="O1234", help="执行中的程序名")
    ap.add_argument("--run-prog", default="1234", type=int, help="运行中的程序号")
    ap.add_argument("--main-prog", default="1234", type=int, help="主程序号")
    ap.add_argument("--line", default="4321", help="顺序号")
    ap.add_argument("--alarm", default="0", help="报警状态位（0 = 无报警）")
    ap.add_argument("--status", default="free",
                    choices=["free", "running", "holding"], help="三态")
    ap.add_argument("--mode", default="auto", choices=["auto", "manual"])
    ap.add_argument("--emergency", default="off", choices=["off", "on"])
    ap.add_argument("--blkcount", default="12345", help="加工块数")
    ap.add_argument("--tool-groups", default="12", help="刀具组数")
    ap.add_argument("--power-minutes", default="90")
    ap.add_argument("--run-minutes", default="65")
    ap.add_argument("--cutting-minutes", default="42")
    ap.add_argument("-v", "--verbose", action="store_true", help="打每帧 hexdump")
    ap.add_argument("--protoforge", default="",
                    help="从 ProtoForge 拉点值，例如 http://127.0.0.1:8000")
    ap.add_argument("--pf-device", default="fanuc", help="ProtoForge 里的设备 id")
    ap.add_argument("--pf-token", default="", help="ProtoForge 的 Bearer token")
    ap.add_argument("--pf-token-file", default="", help="从头一个文件读 token")
    ap.add_argument("--pf-interval", default="500", type=int,
                    help="拉点周期（毫秒，默认 500）")
    args = ap.parse_args(argv)
    machine = Machine(args)
    if args.protoforge:
        token = args.pf_token
        if not token and args.pf_token_file:
            with open(args.pf_token_file, encoding="utf-8") as handle:
                token = handle.read().strip()
        threading.Thread(
            target=poll_protoforge,
            args=(machine, args.protoforge, args.pf_device, token,
                  args.pf_interval, args.verbose),
            daemon=True).start()
        print("[protoforge] 开始拉点：%s/api/v1/devices/%s"
              % (args.protoforge, args.pf_device), flush=True)
    try:
        serve(args.port, machine, args.verbose)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
