#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第二轮：找"载荷最少要多少字节"。
#   RPCRequest 在 0x6211e0 处比：载荷长度 vs 头里 [8..9] 的 u16（=24），
#   不够就报 "short read <载荷长>/24 bytes"。所以载荷至少要 24 字节。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 7080-7090 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import struct
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/JINGDIAO/CNC"


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
        return out[:150]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "✗ %s" % str(data.get("error"))[:130]
    return repr(data.get("value"))


def once(item, spec, body=None):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 7080,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    payload = {"connectionId": conn}
    payload.update(body or {})
    return post(ROOT + "/" + item, payload)


def frame(body, flags=0x01):
    total = 24 + len(body)
    return "ZREP:%d,2:02,3:%02x,24:%s" % (total, flags, body.hex())


def pad(payload, size):
    return payload + b"\x00" * (size - len(payload))


print("=== PART_COUNT：载荷长度扫描（值放前 4 字节 = 1234）")
for size in (4, 8, 16, 20, 23, 24, 32, 40):
    body = pad(struct.pack("<i", 1234), size)
    print("  载荷 %-3d %s" % (size, brief(once("PART_COUNT", frame(body)))))

print("=== 载荷 24 字节，值改一下")
for value in (7, 1234, -1):
    print("  %-6d %s" % (value, brief(once("PART_COUNT",
                                           frame(pad(struct.pack("<i", value),
                                                     24))))))

print("=== 其它端点（载荷 24 字节）")
for item in ("LINE_NUMBER", "PROGRAM", "FEED_SPEED", "FEED_OVERRIDE",
             "SPDL_SPEED", "SPDL_OVERRIDE", "STATUS", "WARNING"):
    print("  %-16s %s" % (item, brief(once(item,
                                           frame(pad(struct.pack("<i", 42),
                                                     24))))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -40 || true
