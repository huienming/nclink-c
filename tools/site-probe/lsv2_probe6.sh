#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 海德汉 LSV2 文件类收尾（07 册 §2.1 的那条尾巴）：
#   GetDirectoryInfo  ← R_DI  → S_DI  + (free_size u32 | 32×4B 属性串 | 32B 属性 | 路径串)
#   GetFileInfo       ← R_FI  → S_FI  + (size u32 | timestamp u32 | attributes u32 | 名字串)
#   GetDirectoryContent ← R_DR 00 → 若干条 S_DR（每条一个文件项，同 S_FI 布局）
#                        —— 每收一条，网关回一条 T_OK 请求；控制侧用它结束整个块传输
#                        （本探针让 T_OK 直接回 T_FD，循环立刻收尾）
#   ChangeDirectory / GetFileList ← C_DC + 路径串 → T_OK，再 R_DI …
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


system_parameters = b"\x00" * 96 + b"\x02\x00" + b"\x10\x00" + b"\x00" * 20

# S_DI：free_size + 32 个 4 字节属性串 + 32 字节属性 + 路径
attributes = bytearray(128)
attributes[0:4] = b"READ"
attributes[4:8] = b"WRIT"
directory = (struct.pack("!L", 65536) + bytes(attributes) + bytes(32)
             + b"TNC:\\\x00")

# S_FI / S_DR：size + timestamp + attributes + 名字（0x02 可改 / 0x20 目录 / 0x40 保护）
file_entry = (struct.pack("!L", 12345) + struct.pack("!L", 0x60000000)
              + struct.pack("!L", 0x02 | 0x20 | 0x40) + b"TEST.H\x00")

reply = "MAP:4:4:" + "|".join([
    "415f4c47=" + block("T_OK"),                     # A_LG
    "525f5652=" + text("S_VR", "TNC640"),            # R_VR 七项通用
    "525f5052=" + block("S_PR", system_parameters),   # R_PR
    "435f4343=" + block("T_OK"),                     # C_CC
    "525f4449=" + block("S_DI", directory),          # R_DI 目录信息
    "525f4649=" + block("S_FI", file_entry),         # R_FI 文件信息
    "525f4452=" + block("S_DR", file_entry),         # R_DR 目录内容（第一条）
    "545f4f4b=" + block("T_FD"),                     # 网关的 T_OK ack → 直接收尾
    "435f4443=" + block("T_OK"),                     # C_DC 换目录
    block("T_OK"),                                   # 兜底
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


def brief(out, limit=300):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-120:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data.get("value") if "value" in data else data)[:limit]


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


out = post("/LSV2/Open/TCP", {"ipAddress": "127.0.0.1", "port": 19000,
                              "timeout": 3})
print("=== /LSV2/Open/TCP  %s" % brief(out, 120))
seen = 0
try:
    conn = (json.loads(out).get("data") or {}).get("connectionId")
except Exception:                                 # noqa: BLE001
    conn = None

for path, extra in (("/LSV2/GetDirectoryInfo", {"path": "/TNC"}),
                    ("/LSV2/ChangeDirectory", {"path": "/TNC"}),
                    ("/LSV2/GetFileInfo", {"path": "/TNC/TEST.H"}),
                    ("/LSV2/GetDirectoryContent", {"path": "/TNC"})):
    body = {"connectionId": conn}
    body.update(extra)
    out = post(path, body)
    print("=== %s %s" % (path, extra))
    print("  %s" % brief(out))
    chunks, seen = dump(seen)
    for raw in chunks[:8]:
        name = raw[4:8].decode("latin-1")
        arg = raw[8:].hex(" ") if len(raw) > 8 else ""
        print("      → %-4s %-3d %s" % (name, len(raw), arg[:80]))
PY

echo "=== 网关日志尾部"
tail -3 "$work/hp2x.log" || true
