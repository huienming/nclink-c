#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
"""寄存器 / 位 / 变量的**读与写**现场核对（21A，2026-09-22；见 10 册 §11.10）。

写这一侧的帧（都从控制器侧 CKrnlAPI 的 In 结构体读出来）：

  0x041B PlcPutRRegister   In { nNo u32, newVal u32 } = 8   Out { hr } -> A = 4
  0x0413 PlcPutIBit        In { nNo u32, newVal u8  } = 8   Out { hr } -> A = 4
  0x0422 NcGlobalPutValue  In { nNo i32, TOcVariant 16 } = 20（值在 [12..]）
     （OCK_TOcVariantToPtr：WriteInt32(dst, nNo) 之后 variant 写在 dst+4，
       类型 i16 在 [4..5]、值在 [12..]）

写值前先读原值，写完读回，最后还原。
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


def hr_of(g):
    if len(g) < 20:
        return (None, None)
    return (struct.unpack_from('<i', g, 12)[0], struct.unpack_from('<i', g, 16)[0])


def reg_read(no):
    g = ask(krnl(0x041A, 8, struct.pack('<I', no)))
    return struct.unpack_from('<i', g, 20)[0]


def reg_write(no, value):
    return ask(krnl(0x041B, 4, struct.pack('<II', no, value)))


def bit_read(code, no):
    g = ask(krnl(code, 8, struct.pack('<I', no)))
    return g[20] if len(g) > 20 else None


def bit_write(code, no, value):
    # In = { nNo u32, newVal u8 } = 8（u8 后面是填充）
    return ask(krnl(code, 4, struct.pack('<IBxxx', no, value & 0xFF)))


def var_read(no):
    g = ask(krnl(0x0421, 20, struct.pack('<I', no)))
    b = g[20:]
    t = struct.unpack_from('<h', b, 0)[0]
    if t == 1:
        return t, struct.unpack_from('<i', b, 8)[0]
    if t == 2:
        return t, struct.unpack_from('<d', b, 8)[0]
    return t, None


def var_write(no, value):
    """In = { nNo, TOcVariant }；整数写类型 1、值放 [12..]。"""
    payload = struct.pack('<Ih6xi', no, 1, value) + b'\x00' * 4  # 值在 [12..]
    assert len(payload) == 20, len(payload)
    return ask(krnl(0x0422, 4, payload))


def main():
    print('=== R 寄存器（0x041A 读 / 0x041B 写）===')
    for no in (771, 3000, 3001):
        base = reg_read(no)
        g = reg_write(no, 123456)
        now = reg_read(no)
        reg_write(no, base)
        back = reg_read(no)
        print('  R%-6d 原值=%-8d 写 123456 -> thr=%s hr=%s 读回=%-8d 还原=%-8d %s'
              % (no, base, hr_of(g)[0], hr_of(g)[1], now, back,
                 'OK' if now == 123456 and back == base else '** 没生效 **'))

    print()
    print('=== 位（0x0412 I / 0x0415 C / 0x0417 S 读，0x0413 PutI / PutC / PutS 写）===')
    for name, get_code, put_code in (('I', 0x0412, 0x0413), ('C', 0x0415, 0x0416),
                                     ('S', 0x0417, 0x0418)):
        base = bit_read(get_code, 0)
        g = bit_write(put_code, 0, 1)
        now = bit_read(get_code, 0)
        bit_write(put_code, 0, base if base is not None else 0)
        back = bit_read(get_code, 0)
        print('  %s0     原值=%-2s 写 1 -> thr=%s hr=%s 读回=%-2s 还原=%-2s'
              % (name, base, hr_of(g)[0], hr_of(g)[1], now, back))

    print()
    print('=== 变量（0x0421 读 / 0x0422 写）===')
    for no in (500, 501, 1, 100):
        base = var_read(no)
        g = var_write(no, 987654)
        now = var_read(no)
        if base[0] == 1:
            var_write(no, base[1])
        back = var_read(no)
        print('  #%-5d 原值=%s 写 987654 -> thr=%s hr=%s 读回=%s 还原=%s %s'
              % (no, base, hr_of(g)[0], hr_of(g)[1], now, back,
                 'OK' if now[1] == 987654 else '** 没生效 **'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
