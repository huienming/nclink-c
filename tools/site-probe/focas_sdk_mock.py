#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""假机床：给 FANUC 自己的 Fwlib（Windows 版 Fwlib64.dll）当对端。

用途和 mock.py 的 `CBREP:` 一样（`focas_data_probe.sh` 那套的 Windows 版），但
这一份能**按 Cb 逐块铺载荷**，用来核"哪个字节是哪个字段"：

    python focas_sdk_mock.py [端口] [--payload HEX] [--ramp] [--size HEX]

  --payload HEX   所有块用同一份载荷（默认：块号 0..15 的"斜坡"，每块 48 字节，
                  第 i 块第 j 字节 = (i*16+j) & 0xFF，一眼能看出偏移）
  --size HEX      块长（默认 0x40；SDK 要求 >= 0x22）
  --blocks N      强制应答块个数（默认 = 请求里的 Cb 个数）。有些调用（例如
                  cnc_absolute ALL_AXES）的数据不在第 0 块，试这个就知道它读第几块。

握手（01 册 §2.2/§2.3 解出来的那套）：
  func 01（hello）→ 16 字节体（[2..4)=0、[8..10)=记录数 0）
  func 21 / 02   → "块个数 + 变长块"，块个数 = 请求体 [0..2)（无体的算 0 → 回 1）
每个块 [8..10) = 0（机床说 OK），[14..16) = 载荷字节数，[16..) = 载荷。

每个请求都打了 hexdump 与 Cb 表（code / arg0 / arg1），所以"某个 SDK 调用发了什么"
直接读日志就有。
"""
import socket
import sys
import threading


def hexdump(data):
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("%04x  %-47s  %s" % (
            i,
            " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return "\n".join(out)


def be16(data, off):
    return int.from_bytes(data[off:off + 2], "big") if len(data) >= off + 2 else 0


def be32(data, off):
    return int.from_bytes(data[off:off + 4], "big") if len(data) >= off + 4 else 0


def cbs(request):
    """请求体里的命令块：[(code, arg0, arg1, 原始 28 字节)]。"""
    out = []
    if len(request) < 12:
        return out
    count = be16(request, 10)
    for i in range(count):
        off = 12 + i * 28
        if len(request) < off + 28:
            break
        cb = request[off:off + 28]
        out.append((be16(cb, 6), be32(cb, 8), be32(cb, 12), cb))
    return out


def ramp(block, size):
    return bytes(((block * 16 + j) & 0xFF) for j in range(max(0, size - 16)))


def cbrep(request, size, payload, adapt, force_blocks):
    count = be16(request, 10) if len(request) >= 12 else 0
    if count < 1:
        count = 1
    if force_blocks:
        count = force_blocks
    blocks = b""
    for i in range(count):
        pl = adapt(i, request) if adapt is not None else (
            payload if payload is not None else ramp(i, size))
        block = bytearray(size)
        block[0:2] = size.to_bytes(2, "big")   # 本块字节数
        block[2:4] = (0).to_bytes(2, "big")    # ecode
        block[8:10] = (0).to_bytes(2, "big")   # 返回码：0 = OK
        keep = min(len(pl), size - 16)
        block[14:16] = keep.to_bytes(2, "big")  # 载荷字节数
        block[16:16 + keep] = pl[:keep]
        blocks += bytes(block)
    body = count.to_bytes(2, "big") + blocks
    head = b"\xa0\xa0\xa0\xa0\x00\x01" + bytes([request[6], 0x02]) + \
        len(body).to_bytes(2, "big")
    return head + body


def run(conn, size, payload, adapt, force_blocks):
    buf = b""
    while True:
        try:
            chunk = conn.recv(4096)
        except OSError:
            return
        if not chunk:
            return
        buf += chunk
        while len(buf) >= 10:
            total = 10 + be16(buf, 8)
            if len(buf) < total:
                break
            frame, buf = buf[:total], buf[total:]
            print("=== request %d bytes  func=0x%02x dir=%d body=%d" %
                  (len(frame), frame[6], frame[7], total - 10), flush=True)
            print(hexdump(frame), flush=True)
            for cb in cbs(frame):
                print("    Cb code=0x%02x (%d) arg0=0x%08x arg1=0x%08x  %s" %
                      (cb[0], cb[0], cb[1], cb[2], cb[3].hex()), flush=True)
            if frame[6] == 0x01:
                body = bytes(16)
                reply = (b"\xa0\xa0\xa0\xa0\x00\x01" + bytes([0x01, 0x02]) +
                         len(body).to_bytes(2, "big") + body)
            else:
                reply = cbrep(frame, size, payload, adapt, force_blocks)
            print("--- reply %d bytes: %s" % (len(reply), reply[:48].hex()),
                  flush=True)
            try:
                conn.sendall(reply)
            except OSError:
                return


def serve(port, size, payload, adapt, force_blocks=0):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(4)
    print("listening on 127.0.0.1:%d  block=0x%x" % (port, size), flush=True)
    while True:
        conn, _ = listener.accept()
        threading.Thread(target=run, args=(conn, size, payload, adapt,
                                           force_blocks),
                         daemon=True).start()


def main(argv):
    port = 8193
    size = 0x40
    payload = None
    blocks = 0
    args = list(argv)

    if args and args[0].isdigit():
        port = int(args.pop(0))
    while args:
        key = args.pop(0)
        if key == "--payload":
            payload = bytes.fromhex(args.pop(0).replace(" ", ""))
        elif key == "--size":
            size = int(args.pop(0), 16)
        elif key == "--ramp":
            payload = None
        elif key == "--blocks":
            blocks = int(args.pop(0), 0)
        else:
            raise SystemExit(__doc__)
    if size < 0x22:
        raise SystemExit("块长至少 0x22（SDK 要读块 [16..34)）")
    serve(port, size, payload, None, blocks)


if __name__ == "__main__":
    main(sys.argv[1:])
