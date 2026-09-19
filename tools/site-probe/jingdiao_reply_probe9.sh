#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第九轮：STATUS 码表 + WARNING 条目布局。
#   STATUS：状态值 = 载荷 [0..3] 的 u32（0 → 'free'，1234 → 'unknown'）。
#   WARNING：条目像"u32 号 + 文本"，这一轮换几种摆法试。
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
import re
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
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        error = str(data.get("error"))
        match = re.search(r"(short read[^\\\"]*|packet type error: \d+|"
                          r"exception recovered: [^\\\"]*)", error)
        return "✗ %s" % (match.group(0) if match else error[-100:])
    return repr(data.get("value"))


def once(item, spec):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 7080,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


def spec(payload):
    total = 48 + len(payload)
    return ("ZREP:%d,2:02,3:01,26:02,27:02,32:%s,48:%s"
            % (total, struct.pack("<H", 24 + len(payload)).hex(),
               payload.hex()))


def blank(size=2500):
    return bytearray(size)


print("=== STATUS 码表（载荷 [0..3] = 值）")
for value in range(0, 12):
    body = blank()
    struct.pack_into("<I", body, 0, value)
    print("  %-3d %s" % (value, brief(once("STATUS", spec(bytes(body))))))

print("=== WARNING：只动 [0..3]，看条目数怎么跟着变")
for value in range(0, 6):
    body = blank()
    struct.pack_into("<I", body, 0, value)
    out = once("WARNING", spec(bytes(body)))
    try:
        entries = json.loads(out)["data"]["value"] or []
        shown = len(entries) if isinstance(entries, list) else entries
    except Exception:                             # noqa: BLE001
        shown = brief(out)
    print("  [0..3]=%-3d 条目数=%s" % (value, shown))

print("=== WARNING：[0..3]=1，[4..7]=0x11111111，[8..11]=0x22222222")
body = blank()
struct.pack_into("<I", body, 0, 1)
struct.pack_into("<I", body, 4, 0x11111111)
struct.pack_into("<I", body, 8, 0x22222222)
print("  %s" % brief(once("WARNING", spec(bytes(body)))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -10 || true
