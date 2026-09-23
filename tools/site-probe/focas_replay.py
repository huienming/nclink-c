#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""把官方 SDK 抓下来的那串帧**用裸 socket 重放一遍**给机床。

用途：本仓库 client 与 SDK 发的字节一模一样、机床却只认 SDK 那一份时，用这支把
"帧不对"与"客户端行为不对"分开 —— 重放能被机床认下，就说明差在客户端这一侧
（时序/套接字），重放也认不下，就说明还差在帧上。01 册 §11.14.4 那条开口项就是这么
定性的（重放同样被机床关连接）。

    python focas_replay.py <host> <port> [帧间停顿秒] [down|up]

`down` = 下行三件套（0x11 / 0x12 / 0x13），`up` = 上行（0x15 / 0x18）。
`down-on-control` = 把下行那三条发到**控制通道**上；`down-on-third` = 另开**第三条**
连接发（两者都是"换个通道试试"的排查手段）。
"""

import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2], 0) if len(sys.argv) > 2 else 8193
# 官方 SDK 是"一个调用一个往返"，帧之间天然有毫秒级间隔；这里可以调。
PAUSE = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05
WHAT = sys.argv[4] if len(sys.argv) > 4 else "down"
DIR = "//CNC_MEM/USER/PATH1/"
PROGRAM = b"O0001\nG01 X100 Y100\nM30\n"


def frame(func, direction, body=b""):
    """一帧：`a0a0a0a0` + type(2) + func + dir + 体长(2) + 体。"""
    return (b"\xa0\xa0\xa0\xa0" + b"\x00\x01" + bytes([func, direction]) +
            len(body).to_bytes(2, "big") + body)


def read_frame(sock, what):
    try:
        head = b""
        while len(head) < 10:
            chunk = sock.recv(10 - len(head))
            if not chunk:
                print("      %s：对端关了连接" % what)
                return None, None
            head += chunk
        length = int.from_bytes(head[8:10], "big")
        body = b""
        while len(body) < length:
            chunk = sock.recv(length - len(body))
            if not chunk:
                print("      %s：体只到 %d/%d 字节" % (what, len(body), length))
                return head, body
            body += chunk
        print("      %s <- func 0x%02x dir %d 体 %d：%s" %
              (what, head[6], head[7], length, body[:32].hex()))
        return head, body
    except socket.timeout:
        print("      %s：超时（机床没回）" % what)
    except OSError as exc:
        print("      %s：%s" % (what, exc))
    return None, None


def send(sock, func, direction, body, what):
    time.sleep(PAUSE)
    print("   -> %s：func 0x%02x dir %d 体 %d" % (what, func, direction, len(body)))
    try:
        sock.sendall(frame(func, direction, body))
    except OSError as exc:
        print("      发不出去：%s" % exc)
        return False
    return True


def start_body(path):
    """start 帧那 516 字节：`[1] = 种类`、`[3] = 1`、`[4..6) = "N:"`、`[6..) = 目录`。"""
    body = bytearray(516)
    body[3] = 0x01
    body[4:6] = b"N:"
    body[6:6 + len(path)] = path.encode()
    return bytes(body)


def main():
    # 官方 SDK 的顺序：先连控制通道、hello 1、拿应答，再连数据通道、hello 2。
    control = socket.create_connection((HOST, PORT), 5)
    control.settimeout(5)
    # 一帧一个 TCP 段（关掉 Nagle）：这台机床的传输那一族像是按"段"解帧的。
    control.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    send(control, 0x01, 1, b"\x00\x01", "hello(控制通道)")
    read_frame(control, "hello1")

    data = socket.create_connection((HOST, PORT), 5)
    data.settimeout(5)
    data.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    send(data, 0x01, 1, b"\x00\x02", "hello(数据通道)")
    read_frame(data, "hello2")

    probe = b"\x00\x01" + b"\x00\x1c\x00\x01\x00\x01\x00\x18" + b"\x00" * 20
    send(data, 0x21, 1, probe, "会话探针")
    read_frame(data, "探针")

    if WHAT == "up":
        send(data, 0x15, 1, start_body(DIR + "O2001"), "0x15 上行 start")
        read_frame(data, "0x15 应答")
        send(data, 0x18, 4, bytes(8), "0x18 数据请求")
        read_frame(data, "0x18 应答")
    elif WHAT in ("down-on-control", "down-on-third"):
        if WHAT == "down-on-third":
            third = socket.create_connection((HOST, PORT), 5)
            third.settimeout(5)
            third.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            send(third, 0x01, 1, b"\x00\x03", "hello(第三条连接)")
            read_frame(third, "hello3")
            data = third
        send(data, 0x11, 1, start_body(DIR), "0x11 下行 start")
        read_frame(data, "0x11 应答")
        send(data, 0x12, 4, PROGRAM, "0x12 数据")
        send(data, 0x13, 1, b"", "0x13 end")
        read_frame(data, "0x13 应答")
    else:
        send(data, 0x11, 1, start_body(DIR), "0x11 下行 start")
        read_frame(data, "0x11 应答")
        send(data, 0x12, 4, PROGRAM, "0x12 数据")
        send(data, 0x13, 1, b"", "0x13 end")
        read_frame(data, "0x13 应答（状态回执，方向 3）")

    control.close()
    data.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
