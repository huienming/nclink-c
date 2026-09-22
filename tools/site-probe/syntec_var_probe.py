#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
"""新代的 PLC / 变量读取能力探针（2026-09-22，21A 模拟器 10.116.54N；见 10 册 §11.9）。

码表来自控制器侧 `Syntec.OpenCNC.OCK_CODE` 的 .cctor（433 个 code，全部取出来了）：

    PlcGetIBit     0x0412  In {nNo}      Out {hr, Value u8}
    PlcGetOBit     0x0414
    PlcGetCBit     0x0415
    PlcGetSBit     0x0417
    PlcGetABit     0x0419
    PlcGetRRegister 0x041A In {nNo}      Out {hr, nValue u32}   <- 注意：就是现成
                                                                 "PART_COUNT/SPDL_SPEED" 用的那个码
    PlcGetTimer    0x041C
    PlcGetCounter  0x041D
    PlcGetCapacity 0x041E  In 空          Out {hr, TPlcCapacity 8*u32}
    NcGlobalGetValue     0x0421 In {nNo}  Out {hr, TOcVariant{nValType,nLongVal,DoubleVal}}
    NcGlobalGetCapacity  0x0423 In 空      Out {hr, nValue u32}
    NcStateGetValue      0x0407 (= 现在的位置/状态区)
    NcCoordStateGetValue 0x04BC In {CoordID, nNo}
    AxisStateGetValue    0x04D4 In {nAxisID, nNo}
"""
import struct
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5566


def krnl(code, size_out, payload=b''):
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


def one(code, n, size_out, payload):
    g = ask(krnl(code, size_out, payload))
    if len(g) < 12:
        return None, None, g
    thr = struct.unpack_from('<i', g, 12)[0] if len(g) >= 16 else None
    hr = struct.unpack_from('<i', g, 16)[0] if len(g) >= 20 else None
    return thr, hr, g


def show(tag, code, payload, size_out, fmt=None):
    thr, hr, g = one(code, None, size_out, payload)
    body = g[20:] if len(g) >= 20 else b''
    extra = ''
    if fmt is not None and len(body) >= struct.calcsize(fmt):
        extra = '  value=%s' % (struct.unpack_from(fmt, body)[0],)
    print('%-28s code=%#06x len=%-4s thr=%-3s hr=%-4s body=%s%s'
          % (tag, code, len(g), thr, hr, body[:12].hex(), extra))
    return hr


print('=== PLC 容量 / 位 / 寄存器 / 定时器 / 计数器 ===')
show('PlcGetCapacity', 0x041E, b'', 8 + 8 * 4)
for name, code in (('I', 0x0412), ('O', 0x0414), ('C', 0x0415), ('S', 0x0417),
                   ('A', 0x0419)):
    show('PlcGet%sBit(0)' % name, code, struct.pack('<I', 0), 4 + 4)
for no in (0, 700, 771, 1000):
    show('PlcGetRRegister(%d)' % no, 0x041A, struct.pack('<I', no), 4 + 4, '<i')
show('PlcGetTimer(0)', 0x041C, struct.pack('<I', 0), 4 + 16)
show('PlcGetCounter(0)', 0x041D, struct.pack('<I', 0), 4 + 16)

print()
print('=== 变量（全局 # / 状态 / 坐标状态 / 轴状态） ===')
show('NcGlobalGetCapacity', 0x0423, b'', 4 + 4, '<i')
for no in (0, 1, 100, 500):
    show('NcGlobalGetValue(%d)' % no, 0x0421, struct.pack('<I', no), 4 + 24)
show('NcStateGetValue(4)', 0x0407, struct.pack('<I', 4), 4 + 4, '<h')
show('NcCoordStateGetValue(0,0)', 0x04BC, struct.pack('<II', 0, 0), 4 + 4)
show('AxisStateGetValue(0,0)', 0x04D4, struct.pack('<II', 0, 0), 4 + 8)
show('NcStateGetCapacity', 0x0408, b'', 4 + 4, '<i')
show('NcDebugGetCapacity', 0x0406 + 0, b'', 4 + 4)
