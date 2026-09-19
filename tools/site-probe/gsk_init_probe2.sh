#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# GSK 的 `Init`（cmd 写命令那条路）最后一格：反汇编 (*GskCnc).Init 0x61ac0c
# 读出来 —— 它把请求载荷 `3f 03 00 01 00 00` 发出去，然后要求**应答载荷正好 6 字节**
# 且等于 `bf 03 00 01 00 00`：第 1 字节是请求第 1 字节的**按位取反**，其它 5 字节照抄
# （`mvn r5,#64` → 0xBF；再 memequal 6 字节）。
# 上一轮只把 8 字节头换序、载荷还是 3f… 所以卡在 `unexpected response data`。
#
# 应答直接用 echo + 打补丁（sn 自动跟着请求走）：EREP:5:64,7:c8,12:bf
#   [5]/[7] 互换 = 头换序，[12] = 0x3f → 0xbf
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


def brief(out):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data.get("value") if "value" in data else data)


def requests():
    log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    out = []
    for block in log.split("--- request")[1:]:
        lines = block.split("\n")
        out.append("".join(line[6:53].replace(" ", "") for line in lines[1:4]
                           if line.strip()))
    return out


opened = post("/GSK/CNC/Open/TCP", {"ipAddress": "127.0.0.1", "port": 6000,
                                    "timeout": 3})
conn = (json.loads(opened).get("data") or {}).get("connectionId")
print("=== GSK/Open/TCP connectionId=%s" % conn)

CANDIDATES = [
    ("echo：头换序 + 载荷首字节取反（本轮的答案）", "EREP:5:64,7:c8,12:bf"),
    ("只换头（上一轮的试法，对照）", "EREP:5:64,7:c8"),
    ("头换序 + 载荷 3f→bf，另加 1 字节（7B）", "EREP:5:64,7:c8,12:bf,18:00"),
    ("头换序 + 载荷首字节 bf 放错位置", "EREP:5:64,7:c8,16:bf"),
]
for label, spec in CANDIDATES:
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    print("  %-40s %s" % (label, brief(post("/GSK/CNC/Init",
                                            {"connectionId": conn}))))
    print("       设备侧 %s" % requests()[-1])
PY

echo "=== 网关日志尾部"
tail -4 "$work/hp2x.log" || true
