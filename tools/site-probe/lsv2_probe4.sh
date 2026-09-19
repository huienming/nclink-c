#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 海德汉 LSV2 第四轮：握手照第三轮答完，再把 R_RI（22/23/24/25/26/27/51）的
# 应答按 pyLSV2 misc.decode_* 的结构喂回去，看网关解出什么 JSON。
# 顺带看 ReadPLC 要什么登录 + 什么 telegram。
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
assert len(system_parameters) == 120

axes = (b"\x00\x03" + b"1.234\x00" + b"-5.678\x00" + b"9.5\x00" +
        b"X\x00" + b"Y\x00" + b"Z\x00")
stack = (struct.pack("!L", 300) + b"MAIN.H\x00" + b"SUB.H\x00")
override = struct.pack("!LLL", 100, 50, 200)
axis_error = (struct.pack("!HH", 1, 2) + struct.pack("!l", 3) + b"TEXT\x00")
tool = (struct.pack("!L", 7) + struct.pack("!H", 1) + struct.pack("!H", 2) +
        struct.pack("<d", 12.5) + struct.pack("<d", 3.25))

# MAP 的键长必须等于请求里"块名 + 参数"的实际字节数：
#   A_LG 16B → 由兜底给 T_OK；R_VR 9B → 键 5 字节；R_PR 8B → 键 4 字节；
#   R_RI 10B → 键 6 字节（块名 4 + 参数 2）
reply = "MAP:4:6:" + "|".join([
    "525f565201=" + text("S_VR", "TNC640"),
    "525f565202=" + text("S_VR", "340595-07"),
    "525f565203=" + text("S_VR", "PLCA"),
    "525f565204=" + text("S_VR", "0x1"),
    "525f565205=" + text("S_VR", "123456"),
    "525f565206=" + text("S_VR", "0"),
    "525f565207=" + text("S_VR", "0"),
    "525f5052=" + block("S_PR", system_parameters),
    "525f52490016=" + s_ri(axes),                      # 22 AXIS_LOCATION
    "525f52490017=" + s_ri(struct.pack("!H", 1)),      # 23 EXEC_STATE
    "525f52490018=" + s_ri(stack),                     # 24 SELECTED_PGM
    "525f52490019=" + s_ri(override),                  # 25 OVERRIDE
    "525f5249001a=" + s_ri(struct.pack("!H", 2)),      # 26 PGM_STATE
    "525f5249001b=" + s_ri(axis_error),                # 27 FIRST_ERROR
    "525f5249001c=" + s_ri(b"\x00" * 9),               # 28 NEXT_ERROR
    "525f52490033=" + s_ri(tool),                      # 51 CURRENT_TOOL
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


def show(chunks):
    for raw in chunks:
        name = raw[4:8].decode("latin-1")
        arg = raw[8:].hex(" ") if len(raw) > 8 else ""
        print("      → %-4s %-3d %s" % (name, len(raw), arg[:100]))


out = post("/LSV2/Open/TCP", {"ipAddress": "127.0.0.1", "port": 19000,
                              "timeout": 3})
print("=== /LSV2/Open/TCP  %s" % brief(out))
seen = 0
chunks, seen = dump(seen)
show(chunks)
try:
    conn = (json.loads(out).get("data") or {}).get("connectionId")
except Exception:                                 # noqa: BLE001
    conn = None

ITEMS = [
    ("/LSV2/GetProgramStatus", {}),
    ("/LSV2/GetProgramStack", {}),
    ("/LSV2/GetExecutionStatus", {}),
    ("/LSV2/GetAxesLocation", {}),
    ("/LSV2/GetOverrideInfo", {}),
    ("/LSV2/GetErrorMessages", {}),
    ("/LSV2/GetSpindleToolStatus", {}),
    ("/LSV2/ReadPLC", {"memoryType": "DWORD", "index": 4448, "size": 1}),
]
for path, extra in ITEMS:
    body = {"connectionId": conn}
    body.update(extra)
    out = post(path, body)
    print("=== %s" % path)
    print("  %s" % brief(out))
    chunks, seen = dump(seen)
    show(chunks)
PY

echo "=== 网关日志尾部"
tail -4 "$work/hp2x.log" || true
