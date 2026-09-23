#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""直接说 FOCAS 帧，问机床一句：这条帧该走**哪条 TCP**。

    python focas_channel_probe.py <host> <port> <mode> [路径] [正文]

mode：
    a11    0x11/0x12/0x13 下发，发在 **hello 计数器 1** 那条上   → 应答（01 册 §11.18）
    b11    同三条帧，发在 **hello 计数器 2** 那条上              → 无应答、连接被断
    ardg   往计数器 1 那条发一条普通命令块（func 0x21 探针）      → 无应答、连接被断
    up15   `0x15`（upstart）→ 反复 `0x18` → `0x19`（upend）        → 看这台机器答不答
    up15d1 同上，但 `0x18` 用 dir 1（不是 dir 4）
    swap   两条连接的 hello 计数器对调，0x11 仍发"先连上的"那条   → 角色跟着计数器走

为什么要它：这台 0i-MF（NCGuide）给两条 TCP 分了工 —— **计数器 1 收传输帧、
计数器 2 收命令帧**，发错那一族机床一声不响就把连接断掉。这个脚本把"除连接以外
一个字节都不改"这件事做到底，所以能把"是帧错了还是道错了"一次分清。

路径/正文：正文按 `unicode_escape` 解，所以 shell 里可以写 `"\\nO0303\\nM30\\n%"`。
"""
import socket
import sys
import time

MAGIC = b"\xa0\xa0\xa0\xa0"


def frame(func, direction, body=b"", ftype=1):
    """a0a0a0a0 + type + func + dir + 体长（大端）+ 体。"""
    return (MAGIC + ftype.to_bytes(2, "big") + bytes([func, direction]) +
            len(body).to_bytes(2, "big") + body)


def sysinfo_body():
    """官方库连上之后紧接着发的那条：func 0x21、一个 code 0x18（ODBSYS）的块。"""
    return (b"\x00\x01"      # 块数
            b"\x00\x1c"      # 块长 28
            b"\x00\x01"      # tag0
            b"\x00\x01"
            b"\x00\x18"      # code
            + b"\x00" * 20)


def start_body(name, kind=0):
    """`cnc_dwnstart4` / `cnc_upstart4` 的那 516 字节定长体（"N:" + 目录/文件名）。"""
    body = bytearray(516)
    body[1] = kind
    body[3] = 1
    body[4] = ord("N")
    body[5] = ord(":")
    body[6:6 + len(name)] = name.encode()
    return bytes(body)


def recv_frame(sock, timeout=5.0):
    """读一帧（先 10 字节头、再按体长读），超时/对端关闭回 (None, 已经收到的)。"""
    sock.settimeout(timeout)
    buf = b""
    try:
        while len(buf) < 10:
            chunk = sock.recv(65536)
            if not chunk:
                return None, buf
            buf += chunk
        n = int.from_bytes(buf[8:10], "big")
        while len(buf) < 10 + n:
            chunk = sock.recv(65536)
            if not chunk:
                return None, buf
            buf += chunk
    except TimeoutError:
        return None, buf
    return buf[:10 + n], buf[10 + n:]


def show(tag, data, extra=b""):
    if data is None:
        print("  %s: 没有应答（对端把连接断了）%s"
              % (tag, (" 尾巴=" + extra.hex()) if extra else ""))
        return None
    print("  %s: type=0x%04x func=0x%02x dir=%d body=%d %s"
          % (tag, int.from_bytes(data[4:6], "big"), data[6], data[7],
             int.from_bytes(data[8:10], "big"), data[10:10 + 24].hex()))
    return data


def connect(host, port, counter, name):
    s = socket.create_connection((host, port), 5)
    s.sendall(frame(0x01, 1, bytes([0, counter & 0xFF])))
    f, _ = recv_frame(s)
    print("%s: hello(%d) -> %s" % (name, counter,
                                   "无应答" if f is None else
                                   "body=%d" % int.from_bytes(f[8:10], "big")))
    return s


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    host = argv[0]
    port = int(argv[1], 0)
    mode = argv[2]
    path = argv[3] if len(argv) > 3 else "//CNC_MEM/USER/PATH1/"
    text = (argv[4].encode().decode("unicode_escape").encode()
            if len(argv) > 4 else b"\nO0303\nG01 X1 Y2\nM30\n%")
    print("== mode=%s -> %s:%d" % (mode, host, port))

    if mode == "swap":
        # 计数器对调：第一条报 2、第二条报 1
        a = connect(host, port, 2, "#1(hello 2)")
        b = connect(host, port, 1, "#2(hello 1)")
    else:
        a = connect(host, port, 1, "#1")
        b = connect(host, port, 2, "#2")
    b.sendall(frame(0x21, 1, sysinfo_body()))
    show("#2 sysinfo", *recv_frame(b))

    if mode == "ardg":
        a.sendall(frame(0x21, 1, sysinfo_body()))
        show("#1 sysinfo", *recv_frame(a))
        return 0

    if mode in ("up15", "up15d1"):
        a.sendall(frame(0x15, 1, start_body(path)))
        if show("#1 0x15 upstart", *recv_frame(a, 8.0)) is None:
            return 1
        d = 1 if mode == "up15d1" else 4
        for ln in (8, 8, 8, 4, 1024, 1400):
            a.sendall(frame(0x18, d, b"\x00" * ln))
            f, _ = recv_frame(a, 2.5)
            if f is None:
                print("  #1 0x18 dir=%d 体%d: 无应答" % (d, ln))
            else:
                print("  #1 0x18 dir=%d 体%d: func=0x%02x dir=%d body=%d %r"
                      % (d, ln, f[6], f[7], int.from_bytes(f[8:10], "big"),
                         f[10:10 + 40]))
        a.sendall(frame(0x19, 1, b""))
        show("#1 0x19 upend", *recv_frame(a, 8.0))
        return 0

    target = a if mode in ("a11", "swap") else b
    which = "#1" if mode in ("a11", "swap") else "#2"
    print("   正文 %d 字节: %r" % (len(text), text))
    target.sendall(frame(0x11, 1, start_body(path)))
    if show("%s 0x11 start" % which, *recv_frame(target, 8.0)) is None:
        return 1
    target.sendall(frame(0x12, 4, text))
    time.sleep(0.05)
    target.sendall(frame(0x13, 1, b""))
    show("%s 0x13 end" % which, *recv_frame(target, 20.0))
    target.sendall(frame(0x02, 1, b""))
    show("%s 0x02 flush" % which, *recv_frame(target, 5.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
