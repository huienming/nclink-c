#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""TCP 抄包器：把"客户端 ↔ 机床"之间的字节原样抄一份下来。

用途：官方 FOCAS 库在**以太网**这条路上不发日志帧（`FWLIBETH.LOG` 只记错误文本，
例如 `recv(PDU body)(580, 3, 1): type 1: dir 3: loc 0: code 4`），真机/NCGuide
回的是什么形状看不见。这一层代理插在中间就能两边都看见：

    python focas_tap.py 8194 127.0.0.1 8193        # 本机 8194 → 机床 8193
    focas_sdk_probe.exe 127.0.0.1 8194 cnc_sysinfo # 探针打到代理上

每个连接一个线程（FOCAS2 以太网会同时开两条 TCP），两个方向各自按 16 字节一行
打 hex + ASCII，前缀 `>>` 是客户端发的、`<<` 是机床回的。帧头 10 字节
（magic a0a0a0a0 + type + func + dir + 体长）另外解析出来打一行，省得自己数。
"""
import socket
import sys
import threading


def hexdump(data):
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("   %04x  %-47s  %s" % (
            i,
            " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return "\n".join(out)


def frame_note(data, who):
    """10 字节帧头：magic / type / func / dir / 体长。"""
    if len(data) < 10 or data[0:4] != b"\xa0\xa0\xa0\xa0":
        return ""
    body = (data[8] << 8) | data[9]
    return ("   %s header: type=0x%04x func=0x%02x dir=%d body=%d\n"
            % (who, (data[4] << 8) | data[5], data[6], data[7], body))


def pump(src, dst, tag, out):
    try:
        while True:
            data = src.recv(4096)
            if not data:
                break
            out.write("%s %d bytes\n%s%s" % (tag, len(data), hexdump(data),
                                             frame_note(data, tag)))
            out.flush()
            dst.sendall(data)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def handle(client, host, port, out, name):
    try:
        server = socket.create_connection((host, port), 5)
    except OSError as exc:
        out.write("!! %s: cannot reach %s:%d (%s)\n" % (name, host, port, exc))
        out.flush()
        client.close()
        return
    out.write("== %s: connected to %s:%d\n" % (name, host, port))
    out.flush()
    t = threading.Thread(target=pump, args=(server, client, "<<", out),
                         daemon=True)
    t.start()
    pump(client, server, ">>", out)
    t.join(5)
    server.close()
    client.close()
    out.write("== %s: closed\n" % name)
    out.flush()


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    listen_port = int(argv[0], 0)
    host = argv[1]
    port = int(argv[2], 0)
    path = argv[3] if len(argv) > 3 else None
    out = open(path, "w", encoding="utf-8") if path else sys.stdout
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", listen_port))
    listener.listen(4)
    print("tap 127.0.0.1:%d -> %s:%d%s"
          % (listen_port, host, port, ("  log=" + path) if path else ""))
    n = 0
    try:
        while True:
            client, _ = listener.accept()
            n += 1
            threading.Thread(target=handle,
                             args=(client, host, port, out, "#%d" % n),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        if path:
            out.close()


if __name__ == "__main__":
    main(sys.argv[1:])
