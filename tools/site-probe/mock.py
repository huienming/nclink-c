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
    SEQ:<hex>|<hex>|...   the n-th request gets the n-th frame
    S7S:<hex>[,<hex>...]  act as an ISO-on-TCP / S7 server (COTP CC + Setup ack
                          + Read ack); one value per requested item
    S7R:<hex>             the same, but the data section is exactly these bytes

Usage: mock.py <port>|<port>-<port>|<p1,p2,...> [reply]
"""
import re
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


def handle(conn, spec):
    conn.settimeout(60.0)
    step = 0
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
            if reply.startswith("SEQ:"):
                parts = reply[4:].split("|")
                chosen = parts[step] if step < len(parts) else parts[-1]
                step += 1
                if chosen:
                    conn.sendall(bytes.fromhex(chosen))
            elif reply.startswith(("S7S:", "S7R:")):
                conn.sendall(s7_reply(data, reply))
            elif reply.startswith("HTTP200:"):
                conn.sendall(http_200(reply[8:]))
            elif reply.startswith("XSUB:"):
                conn.sendall((substitute(data, reply[5:]) + "\n").encode())
            elif reply.startswith("EREP:"):
                out = bytearray(data)
                for item in reply[5:].split(","):
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
