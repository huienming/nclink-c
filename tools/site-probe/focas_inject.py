#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""代理 + 插帧：借官方 SDK 建好的会话，把候选帧**注入**到机床那条连接上。

为什么需要它：上行的数据请求（`0x18 dir 4`）在 NCGuide 上机床一声不响，而本仓库
client / 裸 socket 重放的"传输 start"又会被机床直接关连接 —— 也就是说没法用自己的
客户端去扫这一族帧。这里让**官方 SDK** 去建会话（它的 start 是被答的），代理在中间
把候选帧插进"代理 → 机床"那条 socket，记下机床对每个候选的应答。

    python focas_inject.py <listen> <host> <port> <log> [等多久开始注入(秒，默认 1.0)]

然后跑 SDK（探针）：`focas_sdk_probe64.exe 127.0.0.1 <listen> cnc_upstart4 --shape up4 0 --name O3001`
"""

import socket
import sys
import threading
import time

LISTEN = int(sys.argv[1], 0)
HOST = sys.argv[2]
PORT = int(sys.argv[3], 0)
LOG = sys.argv[4]
DELAY = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0

out = open(LOG, "w", encoding="utf-8")


def say(text):
    out.write(text + "\n")
    out.flush()
    print(text)


def frame(func, direction, body=b""):
    return (b"\xa0\xa0\xa0\xa0" + b"\x00\x01" + bytes([func, direction]) +
            len(body).to_bytes(2, "big") + body)


def recv_frame(sock, timeout):
    """收一帧（超时/断链回 (None, None)）。"""
    sock.settimeout(timeout)
    head = b""
    while len(head) < 10:
        try:
            chunk = sock.recv(10 - len(head))
        except (socket.timeout, OSError):
            return None, None
        if not chunk:
            return None, None
        head += chunk
    length = int.from_bytes(head[8:10], "big")
    body = b""
    while len(body) < length:
        try:
            chunk = sock.recv(length - len(body))
        except (socket.timeout, OSError):
            break
        if not chunk:
            break
        body += chunk
    return head, body


# 候选帧：`0x18`（数据请求）的各种体形状，外加别的功能码/dir。
CANDIDATES = [
    # `0x19` 是**唯一被答的**（回 dir 3 + 码 13 = EW_REJECT），先把它的体扫细。
    ("0x19 dir 4，体 8 个 0", 0x19, 4, bytes(8)),
    ("0x19 dir 4，BE32 长度在前", 0x19, 4, (1024).to_bytes(4, "big") + bytes(4)),
    ("0x19 dir 4，BE32 长度在后", 0x19, 4, bytes(4) + (1024).to_bytes(4, "big")),
    ("0x19 dir 4，BE16 长度在前", 0x19, 4, (512).to_bytes(2, "big") + bytes(6)),
    ("0x19 dir 4，体 = 'O3001'", 0x19, 4, b"O3001\x00\x00\x00"),
    ("0x19 dir 4，体 = 完整路径", 0x19, 4,
     b"//CNC_MEM/USER/PATH1/O3001\x00\x00\x00\x00\x00\x00"),
    ("0x19 dir 4，BE32 长度 + 'O3001'", 0x19, 4,
     (1024).to_bytes(4, "big") + b"O3001\x00\x00\x00\x00"),
    ("0x19 dir 4，体 0", 0x19, 4, b""),
    ("0x19 dir 4，体 4 字节长度", 0x19, 4, (1024).to_bytes(4, "big")),
    ("0x19 dir 1（请求方向）体 8 个 0", 0x19, 1, bytes(8)),
    ("0x1a dir 4，体 8 个 0", 0x1a, 4, bytes(8)),
    ("0x1b dir 4，体 8 个 0", 0x1b, 4, bytes(8)),
    # 对照：SDK 那一帧（`0x18`）与别的功能码
    ("0x18 dir 4，体 8 个 0（SDK 那一帧）", 0x18, 4, bytes(8)),
    ("0x18 dir 1（请求方向）体 8 个 0", 0x18, 1, bytes(8)),
    ("0x17 dir 1（end）体 0", 0x17, 1, b""),
    ("0x16 dir 4，体 8 个 0", 0x16, 4, bytes(8)),
]


def inject_sweep(machine):
    """在机床那条 socket 上逐个发候选帧，记下应答。"""
    say("==== 开始注入 %d 个候选" % len(CANDIDATES))
    for name, func, direction, body in CANDIDATES:
        try:
            machine.sendall(frame(func, direction, body))
        except OSError as exc:
            say("  %-36s 发不出去：%s" % (name, exc))
            return
        head, reply = recv_frame(machine, 1.5)
        if head is None:
            say("  %-36s 无应答" % name)
        else:
            say("  %-36s ★ 有应答：func 0x%02x dir %d 体 %d  %s"
                % (name, head[6], head[7], len(reply or b""),
                   (reply or b"")[:64].hex()))
    say("==== 注入结束")


def main():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", LISTEN))
    listener.listen(4)
    say("代理 127.0.0.1:%d -> %s:%d（注入日志 %s）" % (LISTEN, HOST, PORT, LOG))

    while True:
        client, _ = listener.accept()
        machine = socket.create_connection((HOST, PORT), 5)
        say("== 新会话")

        def c2m():
            try:
                while True:
                    data = client.recv(4096)
                    if not data:
                        break
                    if len(data) > 10 and data[0:4] == b"\xa0\xa0\xa0\xa0":
                        say("   -> 客户端 func 0x%02x dir %d 体 %d"
                            % (data[6], data[7], len(data) - 10))
                    machine.sendall(data)
            except OSError:
                pass

        def m2c():
            try:
                while True:
                    data = machine.recv(4096)
                    if not data:
                        break
                    if len(data) > 10 and data[0:4] == b"\xa0\xa0\xa0\xa0":
                        say("   <- 机床 func 0x%02x dir %d 体 %d"
                            % (data[6], data[7], len(data) - 10))
                    client.sendall(data)
            except OSError:
                pass

        t = threading.Thread(target=c2m, daemon=True)
        t.start()
        threading.Thread(target=m2c, daemon=True).start()
        time.sleep(DELAY)
        inject_sweep(machine)
        time.sleep(0.5)
        client.close()
        machine.close()
        say("== 会话结束")


if __name__ == "__main__":
    main()
