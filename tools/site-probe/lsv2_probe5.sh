#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 海德汉 LSV2 第五轮：三件小事
#   ① 倍率是不是被整除截断（喂 150/250/350 看回什么）；
#   ② 报警列表的结束条件（NEXT_ERROR 回 T_ER + 0x39 = T_ER_NO_NEXT_ERROR 才收尾）；
#   ③ ReadPLC 的 PLCDEBUG 登录之后发什么 telegram。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"

python3 - "$work/reply.txt" <<'PY'
import struct
import sys


def block(name, payload=b""):
    return ("%08x" % len(payload)) + name.encode().hex() + payload.hex()


def text(name, value):
    return block(name, (value + "\x00").encode())


def s_ri(payload):
    return block("S_RI", payload)


system_parameters = b"\x00" * 96 + b"\x02\x00" + b"\x10\x00" + b"\x00" * 20
axes = (b"\x00\x03" + b"1.234\x00" + b"-5.678\x00" + b"9.5\x00" +
        b"X\x00" + b"Y\x00" + b"Z\x00")
error = struct.pack("!HH", 1, 2) + struct.pack("!l", 3) + b"TEXT\x00"

reply = "MAP:4:6:" + "|".join([
    "525f565201=" + text("S_VR", "TNC640"),
    "525f565202=" + text("S_VR", "340595-07"),
    "525f565203=" + text("S_VR", "PLCA"),
    "525f565204=" + text("S_VR", "0x1"),
    "525f565205=" + text("S_VR", "123456"),
    "525f565206=" + text("S_VR", "0"),
    "525f565207=" + text("S_VR", "0"),
    "525f5052=" + block("S_PR", system_parameters),
    "525f52490016=" + s_ri(axes),                       # 22 AXIS_LOCATION
    "525f52490019=" + s_ri(struct.pack("!LLL", 150, 250, 350)),   # 25 OVERRIDE
    "525f5249001b=" + s_ri(error),                      # 27 FIRST_ERROR
    "525f5249001c=" + block("T_ER", b"\x00\x39"),       # 28 NEXT_ERROR -> 57
    block("T_OK"),
])
open(sys.argv[1], "w", encoding="utf-8").write(reply)
print("reply spec %d bytes" % len(reply))
PY

python3 /work/mock.py 19000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

cd "$work/hp2x"
./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=20).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out, limit=260):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-120:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data)[:limit]


def dump(since):
    log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    blocks = log.split("--- request")[1:][since:]
    out = []
    for block in blocks:
        lines = block.split("\n")
        raw = bytearray()
        for line in lines[1:]:
            if not line.strip() or line.startswith("--- "):
                break
            raw += bytes.fromhex(line[6:53].replace(" ", ""))
        out.append(raw)
    return out, since + len(blocks)


def show(chunks, limit=6):
    for raw in chunks[:limit]:
        name = raw[4:8].decode("latin-1")
        arg = raw[8:].hex(" ") if len(raw) > 8 else ""
        print("      → %-4s %-3d %s" % (name, len(raw), arg[:100]))
    if len(chunks) > limit:
        print("      →（共 %d 条）" % len(chunks))


out = post("/LSV2/Open/TCP", {"ipAddress": "127.0.0.1", "port": 19000,
                              "timeout": 3})
print("=== /LSV2/Open/TCP  %s" % brief(out))
seen = 0
chunks, seen = dump(seen)
show(chunks, 20)
try:
    conn = (json.loads(out).get("data") or {}).get("connectionId")
except Exception:                                 # noqa: BLE001
    conn = None

for path, extra in (("/LSV2/GetOverrideInfo", {}),
                    ("/LSV2/GetErrorMessages", {}),
                    ("/LSV2/ReadPLC", {"memoryType": "DWORD", "index": 4448,
                                       "size": 1}),
                    ("/LSV2/ReadPLC", {"memoryType": "BYTE", "index": 0,
                                       "size": 4})):
    body = {"connectionId": conn}
    body.update(extra)
    out = post(path, body)
    print("=== %s %s" % (path, extra or ""))
    print("  %s" % brief(out))
    chunks, seen = dump(seen)
    show(chunks)
PY

echo "=== 网关日志尾部"
tail -4 "$work/hp2x.log" || true
