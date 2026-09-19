#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕：应答帧形状（反汇编 (*CJDMachMon).RPCRequest 0x620dec 得出）
#
#   请求 = 24 字节定长，[22..23] = 项码（PART_COUNT=14 / GetBasicModalInfo=3 /
#   GetRate=15 / GetProgState=2 / GetCncErrList=59），[12..15] = 调用号。
#   应答校验：
#     - 长度 ≥ 24（否则 panic）
#     - [2] 必须是 0x02，否则 "[response(0x02)] packet type error: %d"
#     - [3] 的 bit1 必须为 0（否则走另一条分支）
#     - [8..9]（u16）= 头长（24），载荷 = 应答 [24..]
#   取值：载荷前 4 字节，binary.Read(int32)。
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
        return out[:110]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "✗ %s" % data.get("error")
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


def frame(value, flags=0x01, total=None, extra=b""):
    """24 字节头（拷请求）+ [2]=02、[3]=flags + 载荷。"""
    body = struct.pack("<i", value) + extra
    total = total or 24 + len(body)
    return ("ZREP:%d,2:02,3:%02x,24:%s"
            % (total, flags, body.hex()))


print("=== PART_COUNT（int32，24 字节头 + 4 字节载荷）")
for value in (7, 1234, -1):
    print("  %-6d %s" % (value, brief(once("PART_COUNT", frame(value)))))

print("=== [3] 的 bit1 不清（= 0x03）")
print("  %s" % brief(once("PART_COUNT", frame(7, flags=0x03))))

print("=== 其它端点，同一个应答先试")
for item in ("LINE_NUMBER", "PROGRAM", "FEED_SPEED", "FEED_OVERRIDE",
             "SPDL_SPEED", "SPDL_OVERRIDE", "STATUS", "WARNING"):
    print("  %-16s %s" % (item, brief(once(item, frame(42)))))
PY

echo "=== 请求（前 6 帧）"
grep -A6 -- "--- request" "$work/mock.log" | head -60 || true
