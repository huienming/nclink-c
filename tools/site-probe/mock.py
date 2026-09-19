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


def handle(conn, reply_hex):
    conn.settimeout(5.0)
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            print("--- request %d bytes" % len(data), flush=True)
            print(hexdump(data), flush=True)
            if reply_hex:
                conn.sendall(bytes.fromhex(reply_hex))
                print("--- replied %s" % reply_hex, flush=True)
    except socket.timeout:
        print("--- idle, closing", flush=True)
    except Exception as exc:               # noqa: BLE001
        print("--- error: %s" % exc, flush=True)
    finally:
        conn.close()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8193
    reply = sys.argv[2] if len(sys.argv) > 2 else None
    serve(port, reply)
