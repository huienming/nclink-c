#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 PLC 读应答：按 SetResponse 反汇编出来的形状扫一轮（06 册 §3.0 那一格）。
#
# 关键结论（从 hp2x_box200 的 melsec.(*McCommand).SetResponse 读出来）：
#   - 长度字段在 **headerLen-4** 处，用 encoding/binary.Read 读 2 字节；
#   - 数据从 **headerLen** 处开始拷，长度 = (长度字段 - 2)（十六进制文本模式再 >>1）；
#   - headerLen = 11（应答首字节 bit0=1）或 15（bit0=0）；11 就是标准 3E 应答头。
# 上一轮发的应答少了一个"站号"字节（长度字段落在偏移 6 而不是 7），所以被拒。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 6000,5534 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import re
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
        error = str(data.get("error"))
        return "x %s" % error[-90:]
    return repr(data.get("value") if "value" in data else data)


def last_request():
    log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    block = log.split("--- request")[-1].split("\n")
    return "".join(line[6:53].replace(" ", "") for line in block[1:5]
                   if line.strip())


DATA = "1100220033004400"          # D100..D103 期望 17/34/51/68
CANDIDATES = [
    ("3E-11B  LE len=10", "d00000ffff0300" + "0a00" + "0000" + DATA),
    ("3E-11B  LE len=08", "d00000ffff0300" + "0800" + "0000" + DATA),
    ("3E-11B  BE len=000a", "d00000ffff0300" + "000a" + "0000" + DATA),
    ("3E-10B  (旧)LE len=10", "d00000ffff03" + "0a00" + "0000" + DATA),
    ("3E-15B  LE len=10", "d00000ffff0300" + "00000000" + "0a00" + "0000"
                          + DATA),
    ("3E-15B  BE len=000a", "d00000ffff0300" + "00000000" + "000a" + "0000"
                            + DATA),
    ("4E-13B  序号+LE len=10", "d000000000" + "00ffff03" + "00" + "0a00"
                               + "0000" + DATA),
    ("3E-11B  LE len=10 数据大端", "d00000ffff0300" + "0a00" + "0000"
                                   + "0011002200330044"),
    ("3E-11B  LE len=0c(含结束码)", "d00000ffff0300" + "0c00" + "0000"
                                    + DATA),
    ("首字节 0x51 LE len=10", "510000ffff0300" + "0a00" + "0000" + DATA),
    ("首字节 0x50 LE len=10", "500000ffff0300" + "0a00" + "0000" + DATA),
]


def run(root, label, port, reply, first=False):
    opened = post(root + "/Open/TCP", {"ipAddress": "127.0.0.1",
                                       "port": port, "timeout": 3})
    try:
        conn = (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        conn = None
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(reply)
    out = post(root + "/Read", {"connectionId": conn, "deviceType": "D",
                                "index": 100, "size": 4})
    print("  %-26s %s" % (label, brief(out)))
    if first:
        print("       设备侧请求 %s" % last_request())


for root, port, tag in (("/Mitsubishi/Plc/MC", 6000, "MC"),
                        ("/Mitsubishi/Plc/SLMP", 5534, "SLMP")):
    print("=== %s（%s，端口 %d）" % (tag, root, port))
    for index, (label, reply) in enumerate(CANDIDATES):
        run(root, label, port, reply, first=(index == 0))

print("=== 写应答（沿用上一轮的形状）")
opened = post("/Mitsubishi/Plc/MC/Open/TCP", {"ipAddress": "127.0.0.1",
                                              "port": 6000, "timeout": 3})
conn = (json.loads(opened).get("data") or {}).get("connectionId")
with open(REPLY, "w", encoding="utf-8") as handle:
    handle.write("d00000ffff0302000000")
print("  %s" % brief(post("/Mitsubishi/Plc/MC/Write",
                         {"connectionId": conn, "deviceType": "D", "index": 100,
                          "values": [1, 2, 3, 4]})))
PY

echo "=== 网关日志尾部"
tail -5 "$work/hp2x.log" || true
