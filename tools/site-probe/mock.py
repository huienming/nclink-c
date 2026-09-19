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
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            print("--- request %d bytes" % len(data), flush=True)
            print(hexdump(data), flush=True)
            if reply:
                if reply.startswith("HTTP200:"):
                    conn.sendall(http_200(reply[len("HTTP200:"):]))
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
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8193
    reply = sys.argv[2] if len(sys.argv) > 2 else None
    serve(port, reply)
