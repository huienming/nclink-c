#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 海德汉 LSV2 第三轮：把握手整段答完（A_LG → R_VR 7 项 → R_PR 系统参数 →
# R_CI 系统信息 → C_CC 缓冲区协商），看 Open/TCP 能不能真连上；连上之后
# 逐个方法抓设备侧 telegram。
#
#   telegram = 4 字节大端载荷长 + 4 字节块名 + 载荷（pyLSV2 low_level_com.telegram，
#   块名在偏移 4..8，载荷从偏移 8 开始 —— 别把偏移 8 当成块名）
#   R_PR 的应答载荷 = 120 字节（pyLSV2 misc.decode_system_parameters 的 "!14L8B8L2BH4B2L2HL"）
#   其中 max_block_length 在偏移 98（2 字节大端）
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"

python3 - "$work/reply.txt" <<'PY'
import sys


def block(name, payload_hex=""):
    data = bytes.fromhex(payload_hex)
    return ("%08x" % len(data)) + name.encode().hex() + payload_hex


def text(name, value):
    return block(name, (value + "\x00").encode().hex())


def ascii_hex(text_value):
    return text_value.encode().hex()


system_parameters = ("00" * 96 + "0200" + "1000" + "00" * 20)   # len 120
assert len(bytes.fromhex(system_parameters)) == 120

reply = "MAP:4:5:" + "|".join([
    "415f4c47=" + block("T_OK"),                       # A_LG 登录
    "525f565201=" + text("S_VR", "TNC640"),            # R_VR 01 CONTROL
    "525f565202=" + text("S_VR", "340595-07"),         # R_VR 02 NC_VERSION
    "525f565203=" + text("S_VR", "PLCA"),              # R_VR 03 PLC_VERSION
    "525f565204=" + text("S_VR", "0x1"),               # R_VR 04 OPTIONS
    "525f565205=" + text("S_VR", "123456"),            # R_VR 05 ID
    "525f565206=" + text("S_VR", "0"),                 # R_VR 06 RELEASE_TYPE
    "525f565207=" + text("S_VR", "0"),                 # R_VR 07 SPLC_VERSION
    "525f5052=" + block("S_PR", system_parameters),     # R_PR 系统参数
    "525f4349=" + block("S_CI", "0000000100000001"),    # R_CI 系统信息
    "435f4343=" + block("T_OK"),                       # C_CC 缓冲区协商
    block("T_OK"),                                     # 兜底
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


def brief(out, limit=180):
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
    for index, raw in enumerate(chunks):
        name = raw[4:8].decode("latin-1")
        arg = raw[8:].hex(" ") if len(raw) > 8 else ""
        print("      → %-4s %-3d %s" % (name, len(raw), arg[:120]))


print("=== /LSV2/Open/TCP（整段握手都答）")
out = post("/LSV2/Open/TCP", {"ipAddress": "127.0.0.1", "port": 19000,
                              "timeout": 3})
print("  %s" % brief(out))
seen = 0
chunks, seen = dump(seen)
show(chunks)

try:
    conn = (json.loads(out).get("data") or {}).get("connectionId")
except Exception:                                 # noqa: BLE001
    conn = None
print("  connectionId = %s" % conn)

ITEMS = [
    ("/LSV2/GetVersion", {}),
    ("/LSV2/GetSystemParameter", {}),
    ("/LSV2/GetProgramStatus", {}),
    ("/LSV2/GetProgramStack", {}),
    ("/LSV2/GetExecutionStatus", {}),
    ("/LSV2/GetAxesLocation", {}),
    ("/LSV2/GetOverrideInfo", {}),
    ("/LSV2/GetErrorMessages", {}),
    ("/LSV2/GetSpindleToolStatus", {}),
    ("/LSV2/ReadPLC", {"memoryType": "DWORD", "index": 4448, "size": 1}),
    ("/LSV2/GetDirectoryInfo", {"path": "/TNC"}),
    ("/LSV2/GetDirectoryContent", {"path": "/TNC"}),
    ("/LSV2/GetFileInfo", {"path": "/TNC/test.h"}),
    ("/LSV2/ChangeDirectory", {"path": "/TNC"}),
    ("/LSV2/GetFileList", {"path": "/TNC"}),
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
