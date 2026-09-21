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
  --body HEX      非握手请求一律回这份**裸体**（不做"块个数 + 变长块"那套）。
                  程序上下行（func 0x11/0x12/0x15/0x18）走的就是裸体，用它试
                  "应答里怎么放程序文本/长度"。
  --silent HEXFUNC 对这些功能码**不应答**（程序下行的数据帧 func 0x12 就是这种：
                   驱动发完就走，不回；回了反而把它带歪）。
  --reply-func HEX 应答的 [6] 字节换成这个（缺省是回声请求的 func）。数据帧
                   （上行 0x18）上 SDK 走的是 `dir=4` 那条路，应答的 [6] 要对上它。
  --poselm        每块铺一个**像样的 POSELM/LOADELM**（12 字节：int32 data + dec=3 +
                    unit=0 + disp=1 + name + suff），data 里放块号 ×1000、name 里放
                    'A'+块号 —— 用来问"哪一块是哪一路"（SDK 对 dec/unit 查得严，
                   斜坡载荷会被它判无效清成 0）。
  --almmsg2       每块铺一串**像样的 `cnc_rdalmmsg2` 报警记录**（80 字节一条：
                   `alm_no`@0、文本 `alm_msg[64]`@0x10），用来核这条的应答切法
                   （斜坡载荷会被 SDK 判无效清成 0）。ALMMSG2_MARK=1 时每个字段
                   给可辨识的值，用来找字段偏移。
  --axis-table N  **按块看请求**：Cb `0x89` 那一块回一份像样的轴表（N 根轴，每轴
                   16 字节、前 4 字节轴名），其余块照 `--payload`/斜坡铺。
                   有些调用是"一条请求两个块"（`cnc_rdsvmeter` = `0x56` + `0x89`），
                   `0x89` 回填充字节会被 SDK 判 `-17 EW_PROTOCOL`。

握手（01 册 §2.2/§2.3 解出来的那套）：
  func 01（hello）→ 16 字节体（[2..4)=0、[8..10)=记录数 0）
  func 21 / 02   → "块个数 + 变长块"，块个数 = 请求体 [0..2)（无体的算 0 → 回 1）
每个块 [8..10) = 0（机床说 OK），[14..16) = 载荷字节数，[16..) = 载荷。

每个请求都打了 hexdump 与 Cb 表（code / arg0 / arg1），所以"某个 SDK 调用发了什么"
直接读日志就有。
"""
import socket
import struct
import sys
import threading
import os


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


def poselm(block, size):
    """一块 = 一个像样的 POSELM：data=块号*1000、dec=3、unit=0、disp=1、name='A'+块号。"""
    body = bytearray(max(0, size - 16))
    for k in range(0, len(body) - 11, 12):
        data = (block + 1) * 1000 + k // 12
        body[k:k + 4] = data.to_bytes(4, "big", signed=True)
        body[k + 4:k + 6] = (3).to_bytes(2, "big")
        body[k + 6:k + 8] = (0).to_bytes(2, "big")
        body[k + 8:k + 10] = (1).to_bytes(2, "big")
        body[k + 10] = (ord('A') + block) & 0x7F
        body[k + 11] = 0
    return bytes(body)


def almmsg2(block, size):
    """一块 = 一串 `cnc_rdalmmsg2` 的报警记录。

    **每条 80 字节**，不是 `sizeof(ODBALMMSG2)` 的 76 —— 线上多出 4 个字节，好让
    `alm_msg[64]` 落在记录 +0x10、整条正好 0x50 = 80。已核实的：

        +0x00  alm_no   (BE32)
        +0x10  alm_msg  (64 字节，原样)

    两处证据：现场包那份 Linux `libfwlib32.so` 的 `cnc_rdalmmsg2` 里
    **条数 = 载荷长度 / 80**，`alm_no` 取记录 +0 的 BE32、文本取 +0x10 的 64 字节；
    官方 SDK 跑出来的出参里 `alm_no` 与文本（`out[12..)`）也正好对上这两格。

    **还没钉死的**：中间那三个字段。Linux 库读的是记录 +4（type）/ +8（axis）/
    +0xc（msg_len）—— 也就是记录里 +6/+0xa/+0xe 各空 2 字节，正好铺满 0x50；
    但官方 SDK 的出参没能跟这三格对齐（可能的解释是这版 `fwlib30i64.dll` 的结构体
    与官方头不一致）。这里先按 Linux 库那份铺（它自洽且铺满 80），等下一轮再核。
    见 01 册 §2.6。

    早先按官方头的 76 字节铺，SDK 会把整块判无效、出参一个字节都不写 —— 那不是
    "机床不答"，是形状不合法。
    """
    rec = 80
    mark = os.environ.get("ALMMSG2_MARK") == "1"
    body = bytearray(max(0, size - 16))
    for k in range(0, len(body) - rec + 1, rec):
        n = k // rec + 1
        text = ("ALM %d" % n).encode()
        # ALMMSG2_MARK=1：每个字段给一个"一眼能认出来"的值（核字段偏移用）
        body[k:k + 4] = (0x11223344 if mark else 1000 * block + n).to_bytes(
            4, "big", signed=True)
        body[k + 4:k + 6] = (0x5566 if mark else 1).to_bytes(2, "big")
        body[k + 8:k + 10] = (0x7788 if mark else 0).to_bytes(2, "big")
        body[k + 0xc:k + 0xe] = (0xBBCC if mark else len(text)).to_bytes(
            2, "big")
        body[k + 0x10:k + 0x10 + len(text)] = text
    return bytes(body)


def axis_table(axis_count):
    """轴表（Cb `0x89`）：每轴 16 字节，前 4 字节是轴名，后面 12 字节补 0。

    形状与 `focas_machine.py` 里那份一致（官方库的 `cnc_rdaxisname` 就是把
    `[载荷 + i*16]` 处的 4 字节当轴名 memcpy 出去）。**有些调用（例如
    `cnc_rdsvmeter` = `0x56` + `0x89`）必须两块都答对**，只答 `0x56` 那一条、
    `0x89` 回填充字节的话，SDK 会判 `-17 EW_PROTOCOL` —— 这就是"伺服负载一直没核
    出来"的原因。
    """
    names = "XYZAC"
    out = bytearray()
    for i in range(axis_count):
        name = names[i] if i < len(names) else '?'
        out += name.encode().ljust(4, b"\x00") + b"\x00" * 12
    return bytes(out)


def shaped(base, axis_count):
    """把 `base(i)`（每块铺什么）包一层：**`0x89` 那一块换成像样的轴表**。

    有些调用是"一条请求两个块"，两块都得答对：`cnc_rdsvmeter` 发 `0x56`（伺服负载）
    + `0x89`（轴表），`cnc_rdspmeter` 发 `0x40`×2 + `0x8a`，`cnc_rdposition` 更是
    9 块。只答其中一块、别的回填充字节，SDK 就判 `-17 EW_PROTOCOL` —— 这就是
    "伺服负载/主轴负载一直没核出来"的原因。
    """
    def adapt(index, request):
        table = cbs(request)
        codes = [c[0] for c in table]
        if index >= len(table):
            return base(index)
        code, arg0 = table[index][0], table[index][1]
        if code == 0x0e and arg0 == 0x26F0:
            # 能力块（连接期第 3 条）—— "这台机床几根轴"就在里面（ODBSYS 的
            # `max_axis` @2 + 末尾的 ASCII 轴数），跟 focas_machine.py 铺的一致。
            # 有些调用（cnc_rdsvmeter 就是）按这个数决定回几条记录：这里回填充字节
            # 的话，出参 `data_num` 会一直是 0。
            out = bytearray(struct.pack(">HH", 0x4206, 32))
            out += b" 0 " + b"MD4G249.0" + ("%02d" % axis_count).encode() + b"\x00"
            return bytes(out)
        if index < len(codes) and codes[index] == 0x89:
            return axis_table(axis_count)
        return base(index)
    return adapt


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


def rawrep(request, body, reply_func=None):
    head = b"\xa0\xa0\xa0\xa0\x00\x01" + bytes([reply_func if reply_func is not None
                                                else request[6], 0x02]) + \
        len(body).to_bytes(2, "big")
    return head + body


def run(conn, size, payload, adapt, force_blocks, body, silent, reply_func):
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
            if frame[6] in silent:
                print("--- (silent)", flush=True)
                continue
            if frame[6] == 0x01:
                body = bytes(16)
                reply = (b"\xa0\xa0\xa0\xa0\x00\x01" + bytes([0x01, 0x02]) +
                         len(body).to_bytes(2, "big") + body)
            else:
                # --body 只管"数据"帧（程序上下行是 0x11/0x12/0x15/0x18）：
                # 握手（01/02）与探测（21）还是要走原来的形状，否则连不上。
                # 只管**数据帧**（下行 0x12 / 上行 0x18）：start/end 还是要块形状，
                # 握手 01/02/21 也一样，否则连不上。
                use_raw = body is not None and frame[6] in (0x12, 0x18)
                reply = (rawrep(frame, body, reply_func) if use_raw
                         else cbrep(frame, size, payload, adapt, force_blocks))
            print("--- reply %d bytes: %s" % (len(reply), reply[:48].hex()),
                  flush=True)
            try:
                conn.sendall(reply)
            except OSError:
                return


def serve(port, size, payload, adapt, force_blocks=0, body=None, silent=(),
          reply_func=None, shape=None):
    # shape 给"每块铺什么载荷"的函数；None 就用传入的 payload / 斜坡
    if shape is not None:
        adapt = lambda i, request: shape(i, size)  # noqa: E731
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(4)
    print("listening on 127.0.0.1:%d  block=0x%x" % (port, size), flush=True)
    while True:
        conn, _ = listener.accept()
        threading.Thread(target=run, args=(conn, size, payload, adapt,
                                           force_blocks, body, silent,
                                           reply_func),
                         daemon=True).start()


def main(argv):
    port = 8193
    size = 0x40
    payload = None
    blocks = 0
    body = None
    silent = set()
    shape = None
    reply_func = None
    axis_count = None
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
        elif key == "--body":
            body = bytes.fromhex(args.pop(0).replace(" ", ""))
        elif key == "--silent":
            silent.add(int(args.pop(0), 0))
        elif key == "--reply-func":
            reply_func = int(args.pop(0), 0)
        elif key == "--poselm":
            shape = "poselm"
        elif key == "--almmsg2":
            shape = "almmsg2"
        elif key == "--axis-table":
            axis_count = int(args.pop(0), 0)
        else:
            raise SystemExit(__doc__)
    if size < 0x22:
        raise SystemExit("块长至少 0x22（SDK 要读块 [16..34)）")
    # 每块铺什么：先定"兜底形状"，再（可选）把 0x89 那一块换成轴表。轴表要按**请求**
    # 判断是哪一块，所以走 adapt，而不是 serve 的 shape 参数（那个拿不到请求）。
    if shape == "poselm":
        base = lambda i: poselm(i, size)  # noqa: E731
    elif shape == "almmsg2":
        base = lambda i: almmsg2(i, size)  # noqa: E731
    elif payload is not None:
        base = lambda i: payload  # noqa: E731
    else:
        base = lambda i: ramp(i, size)  # noqa: E731
    adapt = shaped(base, axis_count) if axis_count is not None else None
    serve(port, size, payload, adapt, blocks, body, silent, reply_func)


if __name__ == "__main__":
    main(sys.argv[1:])
