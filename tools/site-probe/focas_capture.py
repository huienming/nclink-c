#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""批抓 FANUC 官方 SDK 的线上帧：`focas_sdk_probe64.exe` → 本机代理 → 机床。

用途：把"某个 cnc_* 调用线上长什么样"（Cb 码 + d/e/arg2/arg3 + 请求载荷）一次
抓一把下来，抄进 clients/focas/focas_codec.c 的 item 表与 focas_values.c 的语义层。
官方库自己不发日志帧，只有把代理插在中间才看得见两边发的是什么。

    python focas_capture.py --host 127.0.0.1 --port 8193 \
        "cnc_wrparam --in 0000000100000000 1 1" \
        "cnc_wrtofs --shape s2_l j 1 1 12345"

每个参数是一条探针调用（照 `focas_sdk_probe64.exe` 的写法，`--in` 铺初值、
`--shape` 临时改调用形状）；跑完打印每条调用发出的命令块。
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import threading

SDK_DIR = r"D:\downloads\focas-test2x64"
PROBE = "focas_sdk_probe64.exe"

CB_SIZE = 28


def parse_frames(data):
    """把一段 TCP 字节流切成 FOCAS 帧（magic + type + func + dir + len）。

    返回 [(func, dir, body)]；不完整/不认识的字节丢掉（代理是双向的，两边各自
    按自己的帧边界走，偶尔会把两条帧并到一个 recv 里）。
    """
    out = []
    i = 0
    while i + 10 <= len(data):
        if data[i:i + 4] != b"\xa0\xa0\xa0\xa0":
            i += 1
            continue
        func = data[i + 6]
        direction = data[i + 7]
        length = (data[i + 8] << 8) | data[i + 9]
        if i + 10 + length > len(data):
            break
        out.append((func, direction, data[i + 10:i + 10 + length]))
        i += 10 + length
    return out


def parse_blocks(body):
    """命令块的 28 字节布局（ncl_focas_cb_write）：size/first/index/code/d/e/a2/a3。

    请求体 = 2 字节块数 + N 个 28 字节块（+ 尾随载荷）。应答体**没有**块数，块长
    以块自己的前 2 字节为准（0x18 那条是 28 字节，读刀补那条是 16 字节）。
    """
    if len(body) < 2:
        return [], b""
    count = (body[0] << 8) | body[1]
    blocks = []
    at = 2
    for _ in range(count):
        if at + CB_SIZE > len(body):
            break
        blk = body[at:at + CB_SIZE]
        blocks.append({
            "code": (blk[6] << 8) | blk[7],
            "first": (blk[2] << 8) | blk[3],
            "index": (blk[4] << 8) | blk[5],
            "d": int.from_bytes(blk[8:12], "big", signed=True),
            "e": int.from_bytes(blk[12:16], "big", signed=True),
            "a2": int.from_bytes(blk[16:20], "big", signed=True),
            "a3": int.from_bytes(blk[20:24], "big", signed=True),
            "tag0": (blk[24] << 8) | blk[25],
        })
        at += CB_SIZE
    return blocks, body[at:]


def parse_reply(body):
    """应答体：按块自己的长度走（第一格就是块长），最后一块之后是载荷。"""
    blocks = []
    at = 0
    while at + 4 <= len(body):
        size = (body[at] << 8) | body[at + 1]
        if size < 4 or at + size > len(body):
            break
        blk = body[at:at + size]
        blocks.append({
            "size": size,
            "code": (blk[6] << 8) | blk[7] if size >= 8 else -1,
            "first": (blk[2] << 8) | blk[3] if size >= 8 else -1,
            "index": (blk[4] << 8) | blk[5] if size >= 8 else -1,
            "tail": blk[8:].hex(),
        })
        at += size
    return blocks, body[at:]


class Tap:
    """8194 → 机床 的转发代理，把"某次调用发了什么"攒下来。"""

    def __init__(self, listen, target):
        self.listen = listen
        self.target = target
        self.lock = threading.Lock()
        self.frames = []          # [(func, dir, body)]
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", listen))
        self.listener.listen(4)

    def clear(self):
        with self.lock:
            self.frames = []

    def take(self):
        with self.lock:
            frames, self.frames = self.frames, []
        return frames

    def add(self, frames):
        with self.lock:
            self.frames.extend(frames)

    def serve(self):
        while True:
            try:
                client, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(client,),
                             daemon=True).start()

    def handle(self, client):
        try:
            server = socket.create_connection(self.target, 5)
        except OSError as exc:
            print("!! 连不上机床 %s: %s" % (self.target, exc))
            client.close()
            return
        threading.Thread(target=self.pump, args=(server, client),
                         daemon=True).start()
        self.pump(client, server)
        try:
            server.close()
        except OSError:
            pass

    def pump(self, src, dst):
        try:
            while True:
                data = src.recv(65536)
                if not data:
                    break
                self.add(parse_frames(data))
                dst.sendall(data)
        except OSError:
            pass
        finally:
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass


def run_probe(host, port, spec):
    parts = spec.split()
    argv = [os.path.join(SDK_DIR, PROBE), "--dll", "Fwlib64.dll", host, str(port)]
    argv += [p for p in parts]
    done = subprocess.run(argv, cwd=SDK_DIR, capture_output=True, text=True,
                          timeout=60)
    return done


def summarize(frames):
    """一条调用的帧：请求（dir 1）里的命令块 + 应答（dir 2）的载荷长度。"""
    lines = []
    for func, direction, body in frames:
        if func == 0x21 and direction == 1:
            blocks, extra = parse_blocks(body)
            lines.append("    >> raw: %s" % body[:80].hex())
            for b in blocks:
                lines.append("    Cb 0x%02x  d=%d e=%d a2=%d a3=%d tag0=%d%s"
                             % (b["code"], b["d"], b["e"], b["a2"], b["a3"],
                                b["tag0"],
                                ("  载荷=" + extra.hex()) if extra else ""))
        elif func == 0x21 and direction == 2:
            blocks, extra = parse_reply(body)
            codes = ",".join("0x%02x/%dB" % (b["code"], b["size"])
                             for b in blocks)
            lines.append("    << 应答 %d 块 [%s]，载荷 %d 字节：%s"
                         % (len(blocks), codes, len(extra), extra[:64].hex()))
            lines.append("    << raw: %s" % body[:72].hex())
        else:
            lines.append("    func 0x%02x dir %d，体 %d 字节"
                         % (func, direction, len(body)))
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("calls", nargs="+", help="探针调用（一条一个）")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8193)
    ap.add_argument("--listen", type=int, default=8194)
    ap.add_argument("--timeout", type=float, default=15.0)
    args = ap.parse_args(argv)

    tap = Tap(args.listen, (args.host, args.port))
    threading.Thread(target=tap.serve, daemon=True).start()

    for spec in args.calls:
        print("== " + spec)
        tap.clear()
        try:
            done = run_probe("127.0.0.1", args.listen, spec)
        except subprocess.TimeoutExpired:
            print("    探针超时")
            continue
        for line in (done.stdout or "").splitlines():
            if line.strip():
                print("    | " + line.rstrip())
        for line in (done.stderr or "").splitlines():
            if line.strip():
                print("    ! " + line.rstrip())
        for line in summarize(tap.take()):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
