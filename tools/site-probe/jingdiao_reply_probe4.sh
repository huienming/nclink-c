#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第三轮：应答是"24 字节一包"的流。
#   第 1 包 = 头（[2]=0x02 类型、[3] 的 bit1 必须为 0、[8..9]=头长 24）；
#   第 2 包必须完整（>=24 字节）且自己也是一个合法包（[2]=0x02）。
#   把第 2 包填成 00 01 02 ... 17，返回的数值就能告诉我们取值偏移。
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
        return out[:130]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "✗ %s" % str(data.get("error"))[:120]
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


def frame(second, first_flags=0x01):
    total = 24 + len(second)
    return "ZREP:%d,2:02,3:%02x,24:%s" % (total, first_flags, second.hex())


PATTERN = bytes(range(24))
print("=== 第 2 包 = 00 01 02 ... 17")
print("  PART_COUNT  %s" % brief(once("PART_COUNT", frame(PATTERN))))
print("  LINE_NUMBER %s" % brief(once("LINE_NUMBER", frame(PATTERN))))

print("=== 第 2 包类型字节改成 0")
bad = bytearray(PATTERN)
bad[2] = 0
print("  PART_COUNT  %s" % brief(once("PART_COUNT", frame(bytes(bad)))))

print("=== 第 2 包 00 01 02 01 04 ...")
alt = bytes([0, 1, 2, 1]) + PATTERN[4:]
print("  PART_COUNT  %s" % brief(once("PART_COUNT", frame(alt))))

print("=== 第 2 包 u16(0) type(2) flags(1) int32(1234)")
rec = struct.pack("<HBBi", 0, 2, 1, 1234) + bytes(12)
print("  PART_COUNT  %s" % brief(once("PART_COUNT", frame(rec))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -20 || true
