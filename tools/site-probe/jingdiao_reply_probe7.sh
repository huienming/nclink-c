#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第七轮：用"递增字节"当载荷，一次问出各数据项的取值偏移。
#   载荷第 k 字节 = k（k mod 256），所以返回值里的字节序列就指明了偏移：
#   例如 LINE_NUMBER 回 0x03020100 就是取载荷 [0..3]，回 0x07060504 就是 [4..7]。
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


PATTERN = bytes(index % 256 for index in range(200))
print("=== 载荷 = 第 k 字节是 k（200 字节）")
for item in ("PART_COUNT", "LINE_NUMBER", "PROGRAM", "FEED_SPEED",
             "FEED_OVERRIDE", "SPDL_SPEED", "SPDL_OVERRIDE", "STATUS"):
    print("  %-16s %s" % (item, brief(once(item, spec(PATTERN)))))

print("=== WARNING（200 字节载荷）")
print("  %s" % brief(once("WARNING", spec(PATTERN))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -12 || true
