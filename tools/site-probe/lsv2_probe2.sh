#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 海德汉 LSV2 第二轮：照 pyLSV2 的约定把握手答上（A_LG → T_OK、R_VR → S_VR），
# 看网关能走到哪一步、随后每个方法发出什么 telegram。
#
#   telegram 形状（pyLSV2 low_level_com.telegram）：4 字节大端载荷长 + 4 字节块名 + 载荷
#   会话：A_LG "INSPECT\0" → R_VR 01（CONTROL）→ R_VR 02（NC_VERSION）→ …
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

# MAP 按请求 [8..12] 的块名（R_VR 再看第 5 字节的参数）挑应答
REPLY='MAP:8:5:525f565201=00000007535f5652544e4336343000|'\
'525f565202=0000000a535f56523334303539352d303700|'\
'415f4c47=00000000545f4f4b|<00000000545f4f4b>'
printf '%s' "$REPLY" >"$work/reply.txt"

python3 /work/mock.py 19000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

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
        out.append((bytes(raw).hex(" "), len(raw)))
    return out, since + len(blocks)


print("=== /LSV2/Open/TCP（握手按 pyLSV2 的约定答）")
out = post("/LSV2/Open/TCP", {"ipAddress": "127.0.0.1", "port": 19000,
                              "timeout": 3})
print("  %s" % brief(out))
seen = 0
chunks, seen = dump(seen)
for index, (chunk, size) in enumerate(chunks):
    print("  设备侧[%d] (%dB) %s" % (index, size, chunk))

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
]
for path, extra in ITEMS:
    body = {"connectionId": conn}
    body.update(extra)
    out = post(path, body)
    print("=== %s" % path)
    print("  %s" % brief(out))
    chunks, seen = dump(seen)
    for index, (chunk, size) in enumerate(chunks):
        print("  设备侧[%d] (%dB) %s" % (index, size, chunk))
PY

echo "=== 网关日志尾部"
tail -5 "$work/hp2x.log" || true
