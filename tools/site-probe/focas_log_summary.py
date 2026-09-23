#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""把 `focas_tap.py` 抄下来的日志按帧过一遍，一帧一行。

用途：比"同一个动作、两个客户端（官方 SDK / 本仓库 client）"的会话差在哪 ——
命令帧打 Cb 码与 d/e/arg2/arg3，应答帧打块返回码。写这一侧（0x09 那一族）实测
出现过"帧一模一样、机床回码不同"的情况，就是靠这个工具把两边的前后文逐帧对齐才
定位到的（01 册 §11.13）。

    python focas_log_summary.py <tap 日志>
"""

import re
import sys

HEAD = re.compile(r"^(\s+)([0-9a-f]{4})\s\s((?:[0-9a-f]{2} ){1,16})")


def chunks(text):
    """把日志里每个 `>> n bytes` / `<< n bytes` 段的十六进制还原成字节。"""
    out = []
    direction = None
    buf = bytearray()
    for line in text.splitlines():
        m = re.match(r"^(>>|<<)\s+(\d+) bytes\s*$", line)
        if m:
            if direction is not None:
                out.append((direction, bytes(buf)))
            direction, _count, buf = m.group(1), int(m.group(2)), bytearray()
            continue
        m = HEAD.match(line)
        if m and direction is not None:
            buf += bytes(int(b, 16) for b in m.group(3).split())
        m = re.match(r"^== (#\d+):", line)
        if m:
            if direction is not None:
                out.append((direction, bytes(buf)))
            direction, buf = None, bytearray()
            out.append(("conn", m.group(1)))
    if direction is not None:
        out.append((direction, bytes(buf)))
    return out


def frames(blob):
    out = []
    at = 0
    while at + 10 <= len(blob):
        if blob[at:at + 4] != b"\xa0\xa0\xa0\xa0":
            break
        func = blob[at + 6]
        direction = blob[at + 7]
        length = (blob[at + 8] << 8) | blob[at + 9]
        if at + 10 + length > len(blob):
            break
        out.append((func, direction, blob[at + 10:at + 10 + length]))
        at += 10 + length
    return out


def describe(func, direction, body):
    who = "->" if direction == 1 else "<-"
    if func == 0x21:
        if direction == 1:
            count = (body[0] << 8) | body[1] if len(body) >= 2 else 0
            items = []
            at = 2
            for _ in range(count):
                if at + 28 > len(body):
                    break
                blk = body[at:at + 28]
                code = (blk[6] << 8) | blk[7]
                d = int.from_bytes(blk[8:12], "big", signed=True)
                e = int.from_bytes(blk[12:16], "big", signed=True)
                a2 = int.from_bytes(blk[16:20], "big", signed=True)
                a3 = int.from_bytes(blk[20:24], "big", signed=True)
                size = (blk[0] << 8) | blk[1]
                items.append("0x%02x d=%d e=%d a2=%d a3=%d size=%d"
                             % (code, d, e, a2, a3, size))
                at += 28
            tail = body[at:].hex()
            return "%s cmd  %s%s" % (who, "; ".join(items),
                                     ("  +载荷 " + tail) if tail else "")
        count = (body[0] << 8) | body[1] if len(body) >= 2 else 0
        items = []
        at = 2
        for _ in range(count):
            if at + 4 > len(body):
                break
            size = (body[at] << 8) | body[at + 1]
            code = (body[at + 6] << 8) | body[at + 7] if size >= 8 else -1
            rc = (body[at + 8] << 8) | body[at + 9] if size >= 10 else -1
            items.append("0x%02x rc=%d size=%d" % (code, rc, size))
            at += size
        return "%s resp %s" % (who, "; ".join(items))
    return "%s func 0x%02x body=%d" % (who, func, len(body))


def main(argv):
    if len(argv) < 1:
        raise SystemExit(__doc__)
    text = open(argv[0], encoding="utf-8", errors="replace").read()
    for direction, payload in chunks(text):
        if direction == "conn":
            print("== %s" % payload)
            continue
        for func, dirn, body in frames(payload):
            print("  %s %s" % (direction, describe(func, dirn, body)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
