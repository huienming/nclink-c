#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""把 tap 日志里官方 SDK 发过的请求帧**原样**重放一遍（一个字节都不改）。

用来把"我的帧和 SDK 的不一样"这条彻底排除掉：直接拿 SDK 的字节重放，机床还不认，
就说明差的不在帧上。

    python focas_replay_verbatim.py <tap 日志> [host] [port]
"""

import re
import socket
import sys
import time

LOG = sys.argv[1]
HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
PORT = int(sys.argv[3], 0) if len(sys.argv) > 3 else 8193


def chunks(path):
    out = []
    cur = None
    conn = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^== (#\d+):", line)
        if m:
            if cur:
                out.append((conn, cur[0], bytes(cur[1])))
                cur = None
            conn = m.group(1)
            continue
        m = re.match(r"^(>>|<<)\s+(\d+) bytes", line)
        if m:
            if cur:
                out.append((conn, cur[0], bytes(cur[1])))
            cur = [m.group(1), bytearray()]
            continue
        m = re.match(r"^\s+([0-9a-f]{4})\s\s((?:[0-9a-f]{2} )+)", line)
        if m and cur:
            cur[1] += bytes(int(b, 16) for b in m.group(2).split())
    if cur:
        out.append((conn, cur[0], bytes(cur[1])))
    return out


def frames(blob):
    out = []
    at = 0
    while at + 10 <= len(blob):
        if blob[at:at + 4] != b"\xa0\xa0\xa0\xa0":
            break
        length = int.from_bytes(blob[at + 8:at + 10], "big")
        if at + 10 + length > len(blob):
            break
        out.append(blob[at:at + 10 + length])
        at += 10 + length
    return out


def main():
    # SDK 那一次会话：控制通道 = 第一对连接的 #1，数据通道 = #2。
    conns = {}
    for conn, direction, blob in chunks(LOG):
        if direction != ">>":
            continue
        conns.setdefault(conn, []).extend(frames(blob))
    order = [c for c in ("#1", "#2") if c in conns]
    if not order:
        print("日志里没有请求帧")
        return 1

    socks = []
    for i, name in enumerate(order):
        sock = socket.create_connection((HOST, PORT), 5)
        sock.settimeout(4)
        socks.append((name, sock))
        for raw in conns[name]:
            func = raw[6]
            print("   %s 发 func 0x%02x dir %d 体 %d：%s" %
                  (name, func, raw[7], len(raw) - 10, raw[10:26].hex()))
            sock.sendall(raw)
            if func in (0x12, 0x18):  # 数据帧不回
                continue
            time.sleep(0.05)
            try:
                head = b""
                while len(head) < 10:
                    piece = sock.recv(10 - len(head))
                    if not piece:
                        print("      -> 对端关了连接")
                        break
                    head += piece
                if len(head) < 10:
                    continue
                length = int.from_bytes(head[8:10], "big")
                body = b""
                while len(body) < length:
                    piece = sock.recv(length - len(body))
                    if not piece:
                        break
                    body += piece
                print("      <- func 0x%02x dir %d 体 %d：%s" %
                      (head[6], head[7], length, body[:32].hex()))
            except socket.timeout:
                print("      <- 超时（机床没回）")
    for _, sock in socks:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
