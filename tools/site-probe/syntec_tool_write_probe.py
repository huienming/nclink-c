#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
"""SYNTEC 刀补读写的现场探针（2026-09-22，21A 模拟器 10.116.54N）。

帧形状来自两段反汇编（见 docs/10 §11.8）：

    [0..3]    Length   = 16 + dwSizeIn
    [4..5]    CmdID    = 16（设备/Dipole 服务）
    [8..11]   Reserved = (0x0700 << 16) | 200，应答原样回显
    [12..15]  uFuncID  = 200（i4；参考客户端把 [14] 留 0）
    [16..19]  dwCode
    [20..23]  dwSizeIn
    [24..27]  dwSizeOut（含 Out 自己的 hr）
    [28..]    In 本体

应答 = 12 字节包头 + 传输层 hr(i4) + Out，而 Out 的第一个字段又是 hr，
所以 [16..19] 是 OK 的 hr，读法的值从 [20..] 起。

用法：
    python syntec_tool_write_probe.py [端口] [刀号]
    默认 5566 / 第 5 把。写完读回来、再把原值写回去。
"""
import socket
import struct
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5566
TOOL = int(sys.argv[2]) if len(sys.argv) > 2 else 5

TOOL_SIZE = 224
IN_SIZE = 4 + TOOL_SIZE          # { nToolNo, TToolOffset }


def krnl(code, size_out, payload, serial=0):
    body = struct.pack('<IIII', 200, code, len(payload), size_out) + payload
    return struct.pack('<IHHI', len(body), 16, 0, 0x070000C8) + body


def ask(frame, timeout=1.0):
    s = socket.create_connection(('127.0.0.1', PORT), 3)
    got = b''
    try:
        s.sendall(frame)
        s.settimeout(timeout)
        while len(got) < 12:
            d = s.recv(12 - len(got))
            if not d:
                break
            got += d
        if len(got) == 12:
            n = struct.unpack_from('<I', got, 0)[0]
            while len(got) < 12 + n:
                d = s.recv(12 + n - len(got))
                if not d:
                    break
                got += d
    finally:
        s.close()
    return got


def read_tool(index):
    g = ask(krnl(0x043F, 4 + TOOL_SIZE, struct.pack('<i', index)))
    return g[20:20 + TOOL_SIZE] if len(g) >= 20 + TOOL_SIZE else None


def put_tool(index, record):
    assert len(record) == TOOL_SIZE
    return ask(krnl(0x0440, 4, struct.pack('<i', index) + record))


def show(tag, rec):
    print('%-16s nose=%-3d radius=%-8.4f rwear=%-8.4f len0=%-8.4f angle=%.2f nz=%d'
          % (tag, struct.unpack_from('<h', rec, 0)[0],
             struct.unpack_from('<d', rec, 8)[0],
             struct.unpack_from('<d', rec, 16)[0],
             struct.unpack_from('<d', rec, 24)[0],
             struct.unpack_from('<d', rec, 216)[0],
             sum(1 for b in rec if b)))


def main():
    base = read_tool(TOOL)
    if base is None:
        print('读不到第 %d 把，先看端口/连接' % TOOL)
        return 1
    show('baseline[%d]' % TOOL, base)
    for other in (TOOL - 1, TOOL + 1):
        if other >= 1:
            show('neighbour[%d]' % other, read_tool(other))

    want = bytearray(TOOL_SIZE)
    struct.pack_into('<h', want, 0, 3)
    want[8:16] = struct.pack('<d', 0.75)
    want[16:24] = struct.pack('<d', 0.25)
    want[24:32] = struct.pack('<d', 1.5)
    want[216:224] = struct.pack('<d', 60.0)
    g = put_tool(TOOL, bytes(want))
    print('write reply %d bytes, transport hr=%d, hr=%d'
          % (len(g), struct.unpack_from('<i', g, 12)[0] if len(g) >= 16 else -1,
             struct.unpack_from('<i', g, 16)[0] if len(g) >= 20 else -1))
    show('after[%d]' % TOOL, read_tool(TOOL))
    for other in (TOOL - 1, TOOL + 1):
        if other >= 1:
            show('neighbour[%d]' % other, read_tool(other))

    put_tool(TOOL, base)
    back = read_tool(TOOL)
    print('restored:', back == base)
    return 0


if __name__ == '__main__':
    sys.exit(main())
