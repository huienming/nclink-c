#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Fake FANUC: log every byte the vendor FOCAS client sends, answer nothing."""
import socket
import sys
import threading
import time


def hexdump(data):
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("%04x  %-47s  %s" % (
            i,
            " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return "\n".join(out)


def serve(port, reply_hex=None):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    print("mock listening on %d" % port, flush=True)
    while True:
        conn, addr = srv.accept()
        print("--- connect from %s" % (addr,), flush=True)
        threading.Thread(target=handle, args=(conn, reply_hex), daemon=True).start()


def listen(port, reply=None):
    """Serve on one port in the background."""
    threading.Thread(target=serve, args=(port, reply), daemon=True).start()


def substitute(request_bytes, template):
    """Fill {uid} / {req} / {reqflip} from the request we just received.

    Modules like KEDE check the reply's <uid> against the request's, so the fake
    machine has to echo it back; the template makes that possible without
    hard-coding a value that changes with every call.
    """
    import re

    text = request_bytes.decode("utf-8", "replace")
    uid = re.search(r"<uid>\s*(\d+)\s*</uid>", text)
    req = re.search(r"<req>\s*(\w+)\s*</req>", text)
    values = {
        "uid": uid.group(1) if uid else "0",
        "req": req.group(1) if req else "no",
    }
    values["reqflip"] = "no" if values["req"].lower() == "yes" else "yes"
    out = template
    for key, value in values.items():
        out = out.replace("{%s}" % key, value)
    return out


def http_200(body):
    raw = body.encode()
    head = ("HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n" % len(raw))
    return head.encode() + raw


def handle(conn, reply):
    # Long idle timeout: the caller may want the connection to stay open while it
    # fires a whole sequence of requests (we only log them, we do not answer).
    conn.settimeout(60.0)
    spec = reply
    step = 0
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            print("--- request %d bytes" % len(data), flush=True)
            print(hexdump(data), flush=True)
            # "@path" re-reads the reply from a file on every request, so a
            # sweep can change the answer between two requests on one
            # connection.
            reply = spec
            # "@path" re-reads the reply from a file on every request, so a
            # sweep can change the answer between two requests on one
            # connection. Resolve it *before* dispatching on the prefix.
            if reply and reply.startswith("@"):
                try:
                    with open(reply[1:], encoding="utf-8") as handle:
                        reply = handle.read().strip()
                except OSError:
                    reply = ""
            if reply and reply.startswith("SEQ:"):
                # SEQ:<hex>|<hex>|... answers the n-th request with the n-th frame
                # (protocols that hand-shake first, like S7: setup, then the read).
                parts = reply[len("SEQ:"):].split("|")
                # Past the end of the list, keep answering with the last frame:
                # many modules re-read on the same connection.
                chosen = parts[step] if step < len(parts) else parts[-1]
                step += 1
                if chosen:
                    conn.sendall(bytes.fromhex(chosen))
                    print("--- replied (step %d)" % step, flush=True)
                continue
            if reply:
                if reply.startswith("HTTP200:"):
                    conn.sendall(http_200(reply[len("HTTP200:"):]))
                elif reply.startswith("XSUB:"):
                    conn.sendall((substitute(data, reply[len("XSUB:"):]) +
                                  "\n").encode())
                elif reply.startswith("EREP:"):
                    # Echo the request with a few bytes replaced:
                    #   EREP:2:02,16:0102ff   -> buf[2]=0x02, buf[16..18]=01 02 ff
                    out = bytearray(data)
                    for item in reply[len("EREP:"):].split(","):
                        off, hexbytes = item.split(":", 1)
                        raw = bytes.fromhex(hexbytes)
                        index = int(off)
                        if index + len(raw) <= len(out):
                            out[index:index + len(raw)] = raw
                    conn.sendall(bytes(out))
                elif reply.startswith("S7S:") or reply.startswith("S7R:"):
                    # A minimal ISO-on-TCP / S7comm server: confirm the COTP
                    # connection, answer Setup Communication, and answer Read Var
                    # with the given value (echoing the request's PDU reference).
                    raw_only = reply.startswith("S7R:")
                    value = bytes.fromhex(reply[4:])
                    cotp_type = data[5] if len(data) > 5 else 0
                    if cotp_type == 0xE0:                # COTP Connection Request
                        params = data[8:]                # echo the CR parameters
                        cc = (bytes([0x11, 0xD0]) + data[6:8] + b"\x00\x01" +
                              params)
                        total = 4 + len(cc)
                        conn.sendall(bytes([0x03, 0x00, total >> 8, total & 0xFF]) +
                                     cc)
                        print("--- replied (COTP CC)", flush=True)
                        continue
                    # COTP DT: TPKT(4) + COTP(3) + S7 header(10)
                    s7 = data[7:]
                    pdu = s7[4:6] if len(s7) >= 6 else b"\x00\x00"
                    function = s7[10] if len(s7) > 10 else 0
                    if function == 0xF0:                 # Setup Communication
                        params = bytes.fromhex("f0000001 0001 01e0".replace(" ", ""))
                        # Ack_Data header: protocol, ROSCTR, redundancy, pdu ref,
                        # param len, data len, error class, error code (the last
                        # two are what makes it an Ack_Data rather than a Job!)
                        body = (b"\x32\x03\x00\x00" + pdu +
                                bytes([0, len(params)]) + b"\x00\x00" +
                                b"\x00\x00" + params)
                        total = 7 + len(body)
                        conn.sendall(bytes([0x03, 0x00, total >> 8, total & 0xFF,
                                            0x02, 0xF0, 0x80]) + body)
                    else:                                # Read Var
                        # This module slices bitlen + 4 bytes, so it reads the
                        # field as a *byte* count (its own quirk, not standard
                        # S7): declare the byte length and see what the value
                        # comes back as.
                        n = len(value)
                        param = bytes([0x04, 0x01, 0xFF, 0x04,
                                       (n >> 8) & 0xFF, n & 0xFF])
                        # S7R: the data section is exactly the bytes given (this
                        # module reverses the first eight of them into a float64,
                        # so feeding reverse(double) makes it return that double).
                        blob = (bytes(value) if raw_only else
                                bytes([0xFF, 0x04, (n >> 8) & 0xFF, n & 0xFF]) +
                                value)
                        if len(value) % 2:
                            blob += b"\x00"
                        blob += value
                        body = (b"\x32\x03\x00\x00" + pdu +
                                bytes([0, len(param)]) +
                                bytes([0, len(blob)]) + b"\x00\x00" +
                                param + blob)
                        total = 7 + len(body)
                        conn.sendall(bytes([0x03, 0x00, total >> 8, total & 0xFF,
                                            0x02, 0xF0, 0x80]) + body)
                    print("--- replied (S7 ack, function 0x%02x)" % function,
                          flush=True)
                elif reply.startswith("TEXT:"):
                    conn.sendall(reply[len("TEXT:"):].encode())
                else:
                    conn.sendall(bytes.fromhex(reply))
                print("--- replied", flush=True)
    except socket.timeout:
        print("--- idle, closing", flush=True)
    except Exception as exc:               # noqa: BLE001
        print("--- error: %s" % exc, flush=True)
    finally:
        conn.close()


if __name__ == "__main__":
    # The port may be a list ("7080,7081") or a range ("7080-7090"): some modules
    # probe a small range before they settle on one (JINGDIAO walks 7080, 7081...).
    spec = sys.argv[1] if len(sys.argv) > 1 else "8193"
    ports = []
    for part in spec.split(","):
        if "-" in part:
            first, last = part.split("-", 1)
            ports.extend(range(int(first), int(last) + 1))
        else:
            ports.append(int(part))
    reply = sys.argv[2] if len(sys.argv) > 2 else None
    if len(ports) == 1:
        serve(ports[0], reply)
    else:
        for extra in ports[1:]:
            listen(extra, reply)
        serve(ports[0], reply)
