#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 兄弟 Brother：抓 GetMaintenanceData / GetToolList 的设备侧请求。
#   Lua 侧（lua_mod/brother_mod.lua）已经写明 param 的含义：
#     9=生产数据2（切削时间 L01） 10=生产数据3（状态历史 C01） 11=生产数据4（C01/T01）
#     12=维护保养 Q01-Q16        23=位置数据（P01/P02/P03/X01）        25=告警 E01
#   Go 侧 GetToolList 的请求 = generalDataPack("%CLOD    ", "ATCTL   00", "\r\n")
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 10000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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
        return urllib.request.urlopen(req, timeout=10).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out, limit=200):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data.get("value") if "value" in data else data)[:limit]


def requests():
    log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    out = []
    for block in log.split("--- request")[1:]:
        lines = block.split("\n")
        raw = bytearray()
        for line in lines[1:]:
            if not line.strip() or line.startswith("--- "):
                break
            raw += bytes.fromhex(line[6:53].replace(" ", ""))
        out.append(bytes(raw))
    return out


opened = post("/Brother/CNC/Open/TCP", {"ipAddress": "127.0.0.1", "port": 10000,
                                       "timeout": 3})
conn = (json.loads(opened).get("data") or {}).get("connectionId")
print("=== Brother/CNC/Open/TCP connectionId=%s" % conn)

seen = 0
for param in (9, 10, 11, 12, 23, 25):
    out = post("/Brother/CNC/GetMaintenanceData", {"connectionId": conn,
                                                  "param": param})
    chunks = requests()[seen:]
    seen += len(chunks)
    print("=== GetMaintenanceData param=%-3d %s" % (param, brief(out)))
    for raw in chunks:
        printable = raw.decode("latin-1").replace("\r", "\\r").replace("\n", "\\n")
        print("      → %-3dB %s" % (len(raw), printable))
        print("        %s" % raw.hex(" "))

out = post("/Brother/CNC/GetToolList", {"connectionId": conn})
chunks = requests()[seen:]
seen += len(chunks)
print("=== GetToolList %s" % brief(out))
for raw in chunks:
    print("      → %-3dB %r" % (len(raw), raw))
    print("        %s" % raw.hex(" "))
PY

echo "=== 网关日志尾部"
tail -4 "$work/hp2x.log" || true
