#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
# -*- coding: utf-8 -*-
"""扫 0x8e 写参数的记录形状

（2026-09-23 核 `cnc_wrparam` 用的探针，原样收进仓库；工作时是在
`D:\downloads\simulators\tools` 下跑的，那里有 `focas_item.exe` 等现场工具。）
"""

import re
import socket
import subprocess
import sys
import threading

EXE = r"D:\downloads\simulators\tools\focas_item.exe"
PORT = 8196
MAGIC = b"\xa0\xa0\xa0\xa0"


class Tap(object):
    def __init__(self):
        self.log = []
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", PORT))
        self.listener.listen(8)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        def pump(src, dst, tag):
            try:
                while True:
                    data = src.recv(65536)
                    if not data:
                        break
                    self.log.append((tag, data))
                    dst.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        n = 0
        while True:
            try:
                client, _ = self.listener.accept()
            except OSError:
                break
            n += 1
            try:
                server = socket.create_connection(("127.0.0.1", 8193), 5)
            except OSError:
                client.close()
                continue
            threading.Thread(target=pump, args=(server, client, "#%d<<" % n),
                             daemon=True).start()
            threading.Thread(target=pump, args=(client, server, "#%d>>" % n),
                             daemon=True).start()

    def ask(self, tag, d=0, e=0, data=None):
        cmd = [EXE, "127.0.0.1", str(PORT), tag, str(d), str(e), "0", "0"]
        if data is not None:
            cmd.append(data.hex() if isinstance(data, (bytes, bytearray)) else data)
        p = subprocess.run(cmd, capture_output=True)
        codes = []
        for t, blob in self.log[-200:]:
            i = 0
            while i + 10 <= len(blob) and blob[i:i + 4] == MAGIC:
                n = int.from_bytes(blob[i + 8:i + 10], "big")
                if i + 10 + n > len(blob):
                    break
                f = blob[i:i + 10 + n]
                if f[6] == 0x21 and f[7] == 2:
                    body = f[10:]
                    if len(body) >= 12 and int.from_bytes(body[8:10], "big") == 0x8e:
                        codes.append(int.from_bytes(body[10:12], "big"))
                i += 10 + n
        return p.stdout.decode("utf-8", "replace"), (codes[-1] if codes else None)

    def echo(self, tag, data=None, d=0, e=0):
        """把我们刚刚发出去的那条请求整帧抓回来。"""
        before = len(self.log)
        out, code = self.ask(tag, d, e, data)
        for t, blob in self.log[before:]:
            i = 0
            while i + 10 <= len(blob) and blob[i:i + 4] == MAGIC:
                n = int.from_bytes(blob[i + 8:i + 10], "big")
                if i + 10 + n > len(blob):
                    break
                f = blob[i:i + 10 + n]
                if f[6] == 0x21 and f[7] == 1:
                    body = f[10:]
                    if int.from_bytes(body[8:10], "big") == (code if code else 0x8e):
                        return out, code, body
                i += 10 + n
        return out, code, None

    def close(self):
        self.listener.close()


def main(argv):
    num, value = int(argv[0]), int(argv[1])
    tap = Tap()
    out, _ = tap.ask("RDPARAM", num, num)
    h = re.search(r"长度 (\d+)：([0-9a-f]*)", out)
    rec = bytes.fromhex(h.group(2))
    print("读记录：%s" % rec[:24].hex(" "))
    v = value.to_bytes(4, "big")

    def mk(base=None, **kw):
        b = bytearray(rec if base is None else base)
        b[8:12] = v
        for k, val in kw.items():
            off = int(k[1:])
            b[off:off + len(val)] = val
        return bytes(b)

    cases = [
        ("原样（号@2、属性@4..8、值@8）", mk()),
        ("@0..2 = 8", mk(**{"o0": b"\x00\x08"})),
        ("@0..2 = 0x108", mk(**{"o0": b"\x01\x08"})),
        ("@4..6 = 0x0300（官方那格）", mk(**{"o4": b"\x03\x00"})),
        ("@6..8 = 0x0000", mk(**{"o6": b"\x00\x00"})),
        ("@12.. = 全 0", mk(base=rec[:12] + b"\x00" * (len(rec) - 12))),
        ("只 12 字节", mk()[:12]),
        ("只 8 字节", mk()[:8]),
        ("@4..8 = 0x00000000（轴 0、类型 0）", mk(**{"o4": b"\x00\x00\x00\x00"})),
        ("@4..8 = 0x00000300", mk(**{"o4": b"\x00\x00\x03\x00"})),
    ]
    for name, payload in cases:
        out, code, body = tap.echo("WRPARAM2", payload)
        size = int.from_bytes(body[2:4], "big") if body else None
        tag1 = int.from_bytes(body[28:30], "big") if body else None
        after, _ = tap.ask("RDPARAM", num, num)
        h2 = re.search(r"长度 (\d+)：([0-9a-f]*)", after)
        got = int.from_bytes(bytes.fromhex(h2.group(2))[8:12], "big")
        print("%-34s 载荷 %-5d 块长 %-5s tag1 %-5s 回码=%-4s 读回@8=%-6d %s"
              % (name, len(payload), size, tag1, code, got,
                 "**落了**" if got == value else ""))
    tap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
