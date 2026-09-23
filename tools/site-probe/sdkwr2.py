#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
# -*- coding: utf-8 -*-
"""抓官方 SDK 的 cnc_wrparam：0x8e 那条请求的记录整段打出来

（2026-09-23 核 `cnc_wrparam` 用的探针，原样收进仓库；工作时是在
`D:\downloads\simulators\tools` 下跑的，那里有 `focas_item.exe` 等现场工具。）
"""

import re
import socket
import subprocess
import sys
import threading

PROBE = r"D:\downloads\focas-test2x64\focas_sdk_probe64.exe"
PORT = 8196
MAGIC = b"\xa0\xa0\xa0\xa0"


class Tap(object):
    def __init__(self):
        self.log = []
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", PORT))
        self.listener.listen(4)
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

    def frames(self):
        out = []
        for tag, data in self.log:
            i = 0
            while i + 10 <= len(data) and data[i:i + 4] == MAGIC:
                n = int.from_bytes(data[i + 8:i + 10], "big")
                if i + 10 + n > len(data):
                    break
                out.append((tag, data[i:i + 10 + n]))
                i += 10 + n
        return out

    def close(self):
        self.listener.close()


def main(argv):
    num, value, length, type_hex = int(argv[0]), int(argv[1]), argv[2], argv[3]
    at = int(argv[4]) if len(argv) > 4 else 4
    seed = bytearray(16)
    seed[0:2] = num.to_bytes(2, "little")
    seed[2:4] = int(type_hex, 0).to_bytes(2, "little")
    seed[at:at + 4] = value.to_bytes(4, "big")
    tap = Tap()
    p = subprocess.run([PROBE, "127.0.0.1", str(PORT), "cnc_wrparam", length,
                        "--in", seed.hex()],
                       cwd=r"D:\downloads\focas-test2x64", capture_output=True)
    out = p.stdout.decode("gbk", "replace")
    rc = [l.split("=")[1].strip() for l in out.splitlines() if l.strip().startswith("rc =")]
    print("length=%s type=%s seed=%s -> SDK rc=%s" % (length, type_hex, seed[:8].hex(),
                                                      rc[0] if rc else "?"))
    for tag, f in tap.frames():
        body = f[10:]
        code = int.from_bytes(body[8:10], "big")
        if f[6] != 0x21:
            continue
        if f[7] == 1 and code == 0x8e:
            print("   0x8e 请求：体 %d，块头 [%s][%s][%s][%s] tag1=%d"
                  % (len(body), body[8:12].hex(), body[12:16].hex(), body[16:20].hex(),
                     body[20:24].hex(), int.from_bytes(body[26:28], "big")))
            print("   记录头 32 字节：%s" % body[30:62].hex(" "))
        elif f[7] == 2 and code == 0x8e:
            print("   机床回码 = %d" % int.from_bytes(body[10:12], "big"))
    tap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
