#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第六轮：报文是"24 字节一包"的链。
#   包1（头，回声请求）：[2]=0x02 类型、[3] 的 bit1 清 0、[8..9]（u16）= 载荷长度；
#   包2（数据包头）：[2]=0x02、[3] 的 bit1 清 0、[8..9] = 端偏移（24+载荷长）；
#   包2 之后是载荷（PART_COUNT 取前 4 字节，int32）。
#
#   证据：包2 的 [8..9] 若写成 4（比 24 小），网关报
#   `slice bounds out of range [24:4]`，说明它做的是 payload[24 : <该 u16>]。
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
        return out[-120:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        error = str(data.get("error"))
        match = re.search(r"(short read[^\\\"]*|packet type error: \d+|"
                          r"exception recovered: [^\\\"]*)", error)
        return "✗ %s" % (match.group(0) if match else error[-100:])
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


def spec(payload, pkt2_len=None, pkt2_flags=0x01, pkt1_flags=0x01):
    """包1 = 回声请求（[2]=02），包2 = 24 字节头 + payload。"""
    pkt2_len = len(payload) if pkt2_len is None else pkt2_len
    total = 48 + len(payload)
    return ("ZREP:%d,2:02,3:%02x,26:02,27:%02x,32:%s,48:%s"
            % (total, pkt1_flags, pkt2_flags,
               struct.pack("<H", pkt2_len).hex(), payload.hex()))


print("=== PART_COUNT：包2 的 [8..9] 扫描（载荷 = int32 1234）")
for length in (4, 24, 24 + 4, 48, 52):
    print("  [8..9]=%-4d %s" % (length, brief(once(
        "PART_COUNT", spec(struct.pack("<i", 1234), pkt2_len=length)))))

print("=== 包2 [3]=0x02（bit1 = 最后一包）时，PART_COUNT 换值确认")
for value in (7, 1234, -1):
    print("  %-6d %s" % (value, brief(once(
        "PART_COUNT", spec(struct.pack("<i", value), 28, pkt2_flags=0x02)))))

print("=== 其它端点（包2 [3]=0x02、载荷 4 字节 = 42）")
for item in ("LINE_NUMBER", "PROGRAM", "FEED_SPEED", "FEED_OVERRIDE",
             "SPDL_SPEED", "SPDL_OVERRIDE", "STATUS", "WARNING"):
    print("  %-16s %s" % (item, brief(once(
        item, spec(struct.pack("<i", 42), 28, pkt2_flags=0x02)))))

print("=== 载荷给大点（64 字节）再看那几家")
for item in ("LINE_NUMBER", "PROGRAM", "FEED_SPEED", "SPDL_SPEED", "STATUS",
             "WARNING"):
    payload = struct.pack("<i", 42) + bytes(60)
    print("  %-16s %s" % (item, brief(once(
        item, spec(payload, 88, pkt2_flags=0x02)))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -12 || true
