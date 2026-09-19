#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 海德汉 LSV2（07 册）：先抓网关发出去的 telegram —— 假机床什么都不回，
# 把 /LSV2/Open/TCP 与各方法的请求字节原样打出来（含握手几步）。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 19000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=15).read().decode("utf-8",
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
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data)


def dump(since):
    """把 mock 日志里 since 之后收到的每个 chunk 打出来（十六进制）。"""
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
        out.append(bytes(raw).hex(" "))
    return out, since + len(blocks)


print("=== /LSV2/Open/TCP（假机床不回）")
out = post("/LSV2/Open/TCP", {"ipAddress": "127.0.0.1", "port": 19000,
                              "timeout": 3})
print("  %s" % brief(out))
seen = 0
chunks, seen = dump(seen)
for index, chunk in enumerate(chunks):
    print("  设备侧[%d] %s" % (index, chunk))

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
]
for path, extra in ITEMS:
    body = {"connectionId": conn}
    body.update(extra)
    out = post(path, body)
    print("=== %s" % path)
    print("  %s" % brief(out))
    chunks, seen = dump(seen)
    for index, chunk in enumerate(chunks):
        print("  设备侧[%d] %s" % (index, chunk))
PY

echo "=== 网关日志尾部"
tail -5 "$work/hp2x.log" || true
