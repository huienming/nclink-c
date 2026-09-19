#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第八轮：收尾三项
#   FEED_SPEED（GetBasicModalInfo 里还用到 float32 类型）→ 用 float32 扫偏移；
#   STATUS → 状态枚举字节在 ~200 附近（[200..203] 动一下就从 unknown 变 free）；
#   WARNING → 载荷 2500 字节，条目像 (u32 号 + 文本)。
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


print("=== FEED_SPEED：float32 = 12.5 逐档试偏移")
for offset in (0, 4, 8, 12, 16, 20, 24, 28, 32):
    body = blank()
    struct.pack_into("<f", body, offset, 12.5)
    print("  off=%-3d %s" % (offset, brief(once("FEED_SPEED", spec(bytes(body))))))

print("=== STATUS：第 200 字节扫值")
for value in range(0, 8):
    body = blank()
    body[200] = value
    print("  [200]=%-2d %s" % (value, brief(once("STATUS", spec(bytes(body))))))
print("=== STATUS：第 201/202/203 字节各试一个值")
for offset in (201, 202, 203, 204):
    body = blank()
    body[offset] = 1
    print("  [%d]=1   %s" % (offset, brief(once("STATUS", spec(bytes(body))))))

print("=== WARNING：全 0 载荷")
print("  %s" % brief(once("WARNING", spec(bytes(blank())))))
print("=== WARNING：[0..3]=2 且 [4..6]='ABC'")
body = blank()
struct.pack_into("<I", body, 0, 2)
body[4:7] = b"ABC"
print("  %s" % brief(once("WARNING", spec(bytes(body)))))
print("=== WARNING：[0..3]=2 且 [8..10]='ABC'")
body = blank()
struct.pack_into("<I", body, 0, 2)
body[8:11] = b"ABC"
print("  %s" % brief(once("WARNING", spec(bytes(body)))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -10 || true
