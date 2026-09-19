#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 两个"非数据"端点收尾：
#   精雕 Bind：应答长度必须**正好 84**，取值在 [36..]（反汇编 (*CJDMachMon).Bind
#              0x620bd4 的 `cmp r3,#84`、0x620c10 的 `len-36`）。
#   GSK Init：走 cmd 写命令那条路——期望头是 65 64 0c c8 0c <sn> 00 00
#             （请求头 65 c8 0c 64 0c <sn> 00 00 的字节对调），载荷 [8..10] = 命令 00 00。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 7080-7090,6000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import re
import struct
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=10).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        error = str(data.get("error"))
        match = re.search(r"(short read[^\\\"]*|packet type error: \d+|"
                          r"unexpected response[^\\\"]*)", error)
        return "✗ %s" % (match.group(0) if match else error[-100:])
    return repr(data.get("value") if "value" in data else data)


def call(root, open_body, item, spec, body=None):
    opened = json.loads(post(root + "/Open/TCP", open_body))
    conn = (opened.get("data") or {}).get("connectionId")
    post(root + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    payload = {"connectionId": conn}
    payload.update(body or {})
    return post(root + "/" + item, payload)


print("=== 精雕 Bind：应答正好 84 字节，取值在 [36..]")
for label, spec in (
        ("84 字节全 0", "ZREP:84,2:02,3:01,26:02,27:02,56:1c000000"),
        ("84 字节 + [36..39]=7", "ZREP:84,2:02,3:01,26:02,27:02,56:1c000000,"
                                 "36:07000000"),
        ("40 字节（对照）", "ZREP:40,2:02,3:01")):
    print("  %-22s %s" % (label, brief(call("/JINGDIAO/CNC",
                                            {"ipAddress": "127.0.0.1",
                                             "port": 7080, "timeout": 3},
                                            "Bind", spec))))

print("=== GSK Init：cmd 写命令那条路（期望头 65 64 0c c8 0c <sn> 00 00）")


def gsk_cmd_frame(sn, cmd, data=b"", header=None):
    """载荷 ≥ 12 字节（shouldResponseCmd 要求），否则报 unexpected response data。"""
    payload = (header or bytes([0x65, 0x64, 0x0C, 0xC8, 0x0C, sn, 0x00, 0x00]))
    payload += bytes([cmd, 0x00, 0x00]) + data
    size = len(payload) + 6
    raw = bytearray(size)
    raw[0], raw[1] = 0x93, 0x00
    struct.pack_into("<H", raw, 2, len(payload))
    raw[4:4 + len(payload)] = payload
    raw[size - 2:] = b"\x55\xaa"
    return raw.hex()


SWAPPED = bytes([0x65, 0x64, 0x0C, 0xC8, 0x0C, 0x01, 0x00, 0x00])
PLAIN = bytes([0x65, 0xC8, 0x0C, 0x64, 0x0C, 0x01, 0x00, 0x00])
for label, spec in (("换序头 + 1B", gsk_cmd_frame(1, 0x3F, bytes(1))),
                    ("换序头 + 5B", gsk_cmd_frame(1, 0x3F, bytes(5))),
                    ("原序头 + 5B（对照）", gsk_cmd_frame(1, 0x3F, bytes(5),
                                                          header=PLAIN)),
                    ("换序头 + 3f0300010000",
                     gsk_cmd_frame(1, 0x3F, bytes.fromhex("0300010000")))):
    print("  %-20s %s" % (label, brief(call("/GSK/CNC",
                                            {"ipAddress": "127.0.0.1",
                                             "port": 6000, "timeout": 3},
                                            "Init", spec))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -30 || true
