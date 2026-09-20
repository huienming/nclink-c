#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""A fake machine: log every byte the gateway sends and answer as told.

The answer is given on the command line (or re-read from a file for sweeps) and
may take several forms:

    <hex>                 send these raw bytes
    TEXT:<text>           send this text
    HTTP200:<body>        send a minimal HTTP 200 with this JSON body
    XSUB:<template>       fill {uid} / {req} / {reqflip} from the request
    EREP:off:hex,...      echo the request, replacing these bytes
    EREP:...,cut:N        same, then truncate the echo to N bytes
    ZREP:N:off:hex,...    a zero-filled N-byte frame with the request's first
                          24 bytes copied in, then patched
    SEQ:<spec>|<spec>|... the n-th request gets the n-th frame (each part may
                          be any other form here, or raw hex)
    LOOPQ:<keyhex>/<filler>/<frame>|<frame>|...  only the requests whose bytes
                          contain keyhex advance; everything else gets
                          <filler>.  For "read the next directory entry until
                          the machine says stop" loops (M70 mochaFSReadDirectory)
    MAP:off:len:<hex>=<spec>|...  pick by the request's bytes [off,off+len):
                          entries with "=" match that hex, a trailing entry
                          without "=" is the default
    S7S:<hex>[,<hex>...]  act as an ISO-on-TCP / S7 server (COTP CC + Setup ack
                          + Read ack); one value per requested item
    S7R:<hex>             the same, but the data section is exactly these bytes
    CBREP:<size>:<payload>  FOCAS: answer with as many response blocks as the
                          request carried Cbs (size in hex, >= 34)

Usage: mock.py <port>|<port>-<port>|<p1,p2,...> [reply]
"""
import re
import socket
import sys
import threading


class LoggedConn:
    """A connection that says how much it sent.

    A reply of the wrong *length* is invisible in the request log and is the
    usual reason a driver stalls or desyncs on the next frame, so every reply
    prints its size and first bytes.
    """

    def __init__(self, conn):
        self.conn = conn

    def sendall(self, payload):
        print("--- sent %d bytes: %s" % (len(payload), payload[:32].hex()),
              flush=True)
        self.conn.sendall(payload)


def hexdump(data):
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("%04x  %-47s  %s" % (
            i,
            " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return "\n".join(out)


def http_200(body):
    raw = body.encode()
    head = ("HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(raw))
    return head.encode() + raw


def substitute(request_bytes, template):
    """Fill {uid} / {req} / {reqflip} from the request we just received."""
    text = request_bytes.decode("utf-8", "replace")
    uid = re.search(r"<uid>\s*(\d+)\s*</uid>", text)
    req = re.search(r"<req>\s*(\w+)\s*</req>", text)
    values = {"uid": uid.group(1) if uid else "0",
              "req": req.group(1) if req else "no"}
    values["reqflip"] = "no" if values["req"].lower() == "yes" else "yes"
    out = template
    for key, value in values.items():
        out = out.replace("{%s}" % key, value)
    return out


def tpkt(cotp):
    total = 4 + len(cotp)
    return bytes([0x03, 0x00, total >> 8, total & 0xFF]) + cotp


def focas_cbrep(request, spec):
    """FOCAS：按请求里的 Cb 个数生成应答块。

    Pdu::getRbPos(i) 在"体 [0..2) = 块个数、块 [0..2) = 本块字节数、块 [8..10) =
    返回码（非 0 抛异常）"这套布局上走，而驱动会按请求的 Cb 个数去取块
    （`cnc_statinfo` 就发 3 个 Cb、要 3 个块），所以个数必须与请求一致。

        CBREP:<块长 hex>[:<载荷 hex>]

    块长至少 34（system_info_v1 会读块 [16..34)）；载荷从块 [16..) 开始铺。
    请求 [10..12) 是 Cb 个数（10 字节的"无体请求"算 0 → 取 1，因为
    Pdu::receive 要求 0x21 的应答块个数非 0）。
    """
    parts = spec[6:].split(":")
    size = int(parts[0], 16)
    payload = bytes.fromhex(parts[1]) if len(parts) > 1 and parts[1] else b""
    count = int.from_bytes(request[10:12], "big") if len(request) >= 12 else 0
    if count < 1:
        count = 1
    block = bytearray(size)
    block[0:2] = size.to_bytes(2, "big")
    room = max(0, size - 16)
    block[16:16 + min(len(payload), room)] = payload[:room]
    body = count.to_bytes(2, "big") + bytes(block) * count
    head = b"\xa0\xa0\xa0\xa0\x00\x01" + bytes([request[6], 0x02]) + \
        len(body).to_bytes(2, "big")
    return head + body


def s7_ack(pdu_ref, params, data):
    """An S7 Ack_Data: 32 03 | redundancy | pdu ref | param len | data len |
    error class | error code | params | data."""
    body = (b"\x32\x03\x00\x00" + pdu_ref +
            bytes([len(params) >> 8, len(params) & 0xFF]) +
            bytes([len(data) >> 8, len(data) & 0xFF]) +
            b"\x00\x00" + params + data)
    return tpkt(b"\x02\xf0\x80" + body)


def s7_reply(request, spec):
    """Answer an S7 request: COTP CC, Setup ack, or Read ack."""
    if len(request) > 5 and request[5] == 0xE0:           # COTP CR -> CC
        params = request[8:]                              # echo the CR params
        cc = bytes([0x11, 0xD0]) + request[6:8] + b"\x00\x01" + params
        return tpkt(cc)
    if len(request) < 17:
        return b""
    s7 = request[7:]                                      # S7 header + body
    pdu_ref = s7[4:6]
    function = s7[10]
    if function == 0xF0:                                  # Setup Communication
        params = bytes.fromhex("f0000001 0001 01e0".replace(" ", ""))
        return s7_ack(pdu_ref, params, b"")

    raw_only = spec.startswith("S7R:")
    body = spec[4:]
    if raw_only or "," not in body:
        values = [bytes.fromhex(body)]
    else:
        values = [bytes.fromhex(part) for part in body.split(",")]

    if raw_only:
        # the data section is exactly these bytes (one value, however many items
        # the request asked for)
        n = len(values[0])
        params = bytes([0x04, 0x01, 0xFF, 0x04, n >> 8, n & 0xFF])
        return s7_ack(pdu_ref, params, values[0])

    count = s7[11] if len(s7) > 11 else 1
    if count == 0 or count > 32:
        count = 1
    params = bytearray([0x04, count])
    data = bytearray()
    for index in range(count):
        item = values[min(index, len(values) - 1)]
        n = len(item)
        params += bytes([0xFF, 0x04, n >> 8, n & 0xFF])
        data += bytes([0xFF, 0x04, n >> 8, n & 0xFF]) + item
    return s7_ack(pdu_ref, bytes(params), bytes(data))


def respond(conn, data, reply):
    """Send one reply for one received chunk (specs may nest inside SEQ)."""
    if not reply:
        return
    if reply.startswith("SEQ:"):
        return                       # 由 handle 处理（要按请求序号挑）
    if reply.startswith("LOOPQ:"):
        return                       # 由 handle 处理（要按请求内容挑）
    if reply.startswith("MAP:"):
        _, off, size, rest = reply.split(":", 3)
        key = data[int(off):int(off) + int(size)].hex()
        chosen = None
        for item in rest.split("|"):
            if "=" in item:
                match, candidate = item.split("=", 1)
                if match.strip() == key:
                    chosen = candidate
                    break
            else:
                chosen = item             # 兜底（放最后）
        return respond(conn, data, chosen or "")
    if reply.startswith(("S7S:", "S7R:")):
        conn.sendall(s7_reply(data, reply))
    elif reply.startswith("CBREP:"):
        conn.sendall(focas_cbrep(data, reply))
    elif reply.startswith("HTTP200:"):
        conn.sendall(http_200(reply[8:]))
    elif reply.startswith("XSUB:"):
        conn.sendall((substitute(data, reply[5:]) + "\n").encode())
    elif reply.startswith("EREP:"):
        out = bytearray(data)
        cut = None
        for item in reply[5:].split(","):
            if item.startswith("cut:"):
                cut = int(item[4:])
                continue
            off, hexbytes = item.split(":", 1)
            raw = bytes.fromhex(hexbytes)
            index = int(off)
            if index + len(raw) <= len(out):
                out[index:index + len(raw)] = raw
        if cut is not None:
            out = out[:cut]
        conn.sendall(bytes(out))
    elif reply.startswith("ZREP:"):
        out = bytearray(int(reply[5:].split(",")[0]))
        out[:min(24, len(data))] = data[:24]
        for item in reply[5:].split(",")[1:]:
            off, hexbytes = item.split(":", 1)
            raw = bytes.fromhex(hexbytes)
            index = int(off)
            if index + len(raw) <= len(out):
                out[index:index + len(raw)] = raw
        conn.sendall(bytes(out))
    elif reply.startswith("TEXT:"):
        conn.sendall(reply[5:].encode())
    else:
        conn.sendall(bytes.fromhex(reply))


def handle(conn, spec):
    conn.settimeout(60.0)
    step = 0
    loop = 0
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            print("--- request %d bytes" % len(data), flush=True)
            print(hexdump(data), flush=True)
            reply = spec
            if reply and reply.startswith("@"):
                try:
                    with open(reply[1:], encoding="utf-8") as handle_file:
                        reply = handle_file.read().strip()
                except OSError:
                    reply = ""
            if not reply:
                continue
            if reply.startswith("LOOPQ:"):
                keyhex, filler, rest = reply[6:].split("/", 2)
                if keyhex and bytes.fromhex(keyhex) in data:
                    parts = rest.split("|")
                    chosen = parts[loop] if loop < len(parts) else parts[-1]
                    loop += 1
                else:
                    chosen = filler
                respond(LoggedConn(conn), data, chosen)
                print("--- replied", flush=True)
                continue
            if reply.startswith("SEQ:"):
                parts = reply[4:].split("|")
                chosen = parts[step] if step < len(parts) else parts[-1]
                step += 1
                respond(LoggedConn(conn), data, chosen)
            else:
                respond(LoggedConn(conn), data, reply)
            print("--- replied", flush=True)
    except socket.timeout:
        print("--- idle, closing", flush=True)
    except Exception as exc:                             # noqa: BLE001
        print("--- error: %s" % exc, flush=True)
    finally:
        conn.close()


def serve(port, spec):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    print("mock listening on %d" % port, flush=True)
    while True:
        conn, addr = srv.accept()
        print("--- connect from %s" % (addr,), flush=True)
        threading.Thread(target=handle, args=(conn, spec), daemon=True).start()


def listen(port, spec):
    threading.Thread(target=serve, args=(port, spec), daemon=True).start()


def parse_ports(text):
    ports = []
    for part in text.split(","):
        if "-" in part:
            first, last = part.split("-", 1)
            ports.extend(range(int(first), int(last) + 1))
        else:
            ports.append(int(part))
    return ports


if __name__ == "__main__":
    ports = parse_ports(sys.argv[1] if len(sys.argv) > 1 else "8193")
    spec = sys.argv[2] if len(sys.argv) > 2 else None
    for extra in ports[1:]:
        listen(extra, spec)
    serve(ports[0], spec)
