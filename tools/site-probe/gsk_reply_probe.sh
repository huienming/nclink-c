#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 广州数控 GSK：应答头（反汇编 (*GskCnc).query / .cmd / .shouldResponseCmd 得出）
#
#   shouldResponseCmd 先查帧壳（93 00 <len:2 LE> <载荷> 55 aa，总长 = len+6，
#   且 len ≥ 12），再把"应答载荷前 8 字节"和驱动自己算的*期望头*做 memequal（8 字节）。
#   读命令（query）的期望头 = `00 64 1e c8 1e 17 10 01`
#   （对照请求头 `6f c8 1e 64 1e 17 10 17`：第 1..3 字节反序、末字节变 01、首字节清零）。
#   载荷第 8 字节起 = 命令字节（子码命令再加 1 字节子码），之后是数据。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 6000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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
ROOT = "/GSK/CNC"


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
        return "✗ %s" % str(data.get("error"))[:110]
    return repr(data.get("value"))


def once(item, spec):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 6000,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


HEADER = "00641ec81e171001"


def frame(cmd, data=b""):
    """93 00 <len> <载荷> 55 aa；载荷 = 期望头 8B + 命令 + 两个 0 + 数据。

    "命令 + 00 00"这三字节是每个 getter 自己再比一次的（如 GetCncState
    0x61d590、GetFeedSpeedAct 0x61e1e4），子码命令这里是 `1a 00 00`（**不带**
    请求里的子码）。数据从载荷 [11] 起。
    """
    payload = bytes.fromhex(HEADER) + bytes([cmd]) + b"\x00\x00" + data
    size = len(payload) + 6
    raw = bytearray(size)
    raw[0], raw[1] = 0x93, 0x00
    struct.pack_into("<H", raw, 2, len(payload))
    raw[4:4 + len(payload)] = payload
    raw[size - 2:] = b"\x55\xaa"
    return raw.hex()


CMDS = {
    "STATUS": 0x11,
    "PART_COUNT": 0x16,
    "PROGRAM": 0x12,
    "LINE_NUMBER": 0x23,
    "FEED_SPEED": 0x1A,
    "SPDL_SPEED": 0x1A,
    "FEED_OVERRIDE": 0x1A,
    "SPDL_OVERRIDE": 0x1A,
    "RAPID_OVERRIDE": 0x1A,
    "TOOL_NUMBER": 0x17,
    "WARNING": 0x81,
}

print("=== 速度/倍率类：数据摆法试三种（float64 / float32 / u32 1234）")
for label, data in (("float64 12.5", struct.pack("<d", 12.5)),
                    ("float32 12.5", struct.pack("<f", 12.5)),
                    ("u32 1234", struct.pack("<I", 1234)),
                    ("u32 1234+u32 5678", struct.pack("<II", 1234, 5678))):
    row = []
    for item in ("FEED_SPEED", "SPDL_SPEED", "FEED_OVERRIDE", "SPDL_OVERRIDE",
                 "RAPID_OVERRIDE"):
        row.append("%s=%s" % (item.split("_")[0],
                              brief(once(item, frame(0x1A, data)))))
    print("  %-18s %s" % (label, "  ".join(row)))

print("=== STATUS：数据第 0 字节 = 0..4")
for value in range(0, 5):
    print("  %-3d %s" % (value, brief(once("STATUS", frame(0x11,
                                                          bytes([value]))))))

print("=== WARNING：数据 = 16 个 0（应为空表）")
print("  %s" % brief(once("WARNING", frame(0x81, bytes(16)))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -12 || true
