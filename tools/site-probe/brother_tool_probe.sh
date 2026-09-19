#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 兄弟 Brother：GetToolList / GetMaintenanceData 的应答体试几种排法。
#   请求（🟢 实测，26B 定长）：%CLOD    <命令 6 字符左对齐> 00 \r\n \r\n 01 %
#     GetToolList → ATCTL；GetMaintenanceData → 9:PRDC2 10:PRD3 11:MONTR 12:MAINTC
#                                          23:PDSP 25:ALARM
#   应答形状（11 册 §3.2 已知）：<请求第一行 19 字节原样> \r\n <数据> \r\n %
#   Lua 侧要的形状：data.value["X01"][1..13] / ["C01"] / ["T01"] / ["P01..P03"] / ["M01".."M51"]
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


def brief(out, limit=300):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %r" % str(data.get("error"))[:140]
    return repr(data.get("value") if "value" in data else data)[:limit]


def reply_text(data):
    """ZREP：把应答的**前 24 字节**写成"请求原样"，再把 [19:21] 改成 CRLF、
    [21:] 放数据、末尾补 CRLF + `%` —— 第一段必须是**真实请求行**，手写字符串不行
    （PWD 的真实请求是 `%CFLDPWD         00`，不是 `%CLOD    PWD     00`）。"""
    raw = data.encode()
    with open(REPLY, "w", encoding="latin-1") as handle:
        handle.write("ZREP:%d,19:0d0a,21:%s,%d:0d0a25"
                     % (24 + len(raw), raw.hex(), 21 + len(raw)))


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


# responseIsOk（0x617704）的三条硬规矩（11 册 §3.2）：
#   ① 按 "\r\n" 切 ≥2 段；② 第 1 段 ≥19 字节且以 "00" 结尾；③ 整条以 "%" 收尾
# filterResData（0x617324）里"键/值"的分隔符是**逗号**（0x7e3193 那格）
CANDIDATES = [
    ("逗号分隔 5 字段 M01", "M01,1,0,0,1,0,0000"),
    ("逗号分隔 M01..M03", "M01,1,0,0,1,0\r\nM02,2,0,0,1,0\r\nM03,0,0,0,0,0"),
    ("M03 首字段改 3（看 0 是不是被丢）",
     "M01,1,0,0,1,0\r\nM02,2,0,0,1,0\r\nM03,3,0,0,1,0"),
    ("末尾加一行 00 当结束行",
     "M01,1,0,0,1,0\r\nM02,2,0,0,1,0\r\nM03,3,0,0,1,0\r\n00"),
    ("末尾加一行空+00", "M01,1,0,0,1,0\r\nM02,2,0,0,1,0\r\n\r\n00"),
    ("每行 6 个字段（key + 6）", "M01,1,0,0,1,0,0"),
    ("每行 4 个字段（key + 4）", "M01,1,0,0,1"),
    ("空格分隔 M01", "M01 1 0 0 1 0"),
    ("逗号 6 字段（含位号）", "M01,1,001,000,001,000,000"),
    ("分号分隔", "M01;1;0;0;1;0"),
    ("T01 形式", "T01,1,0,0,1,0"),
]

opened = post("/Brother/CNC/Open/TCP", {"ipAddress": "127.0.0.1", "port": 10000,
                                       "timeout": 3})
conn = (json.loads(opened).get("data") or {}).get("connectionId")
print("=== Brother/CNC/Open/TCP connectionId=%s" % conn)

print("=== 对照：PWD（11 册 §3.2 说这条以前就通）")
with open(REPLY, "w", encoding="latin-1") as handle:
    handle.write("")
before = len(requests())
post("/Brother/CNC/PWD", {"connectionId": conn})
for raw in requests()[before:]:
    print("  真实 PWD 请求 %r" % raw)
for label, body in (("请求行 + 数据 + %", "%CLOD    PWD     00\r\n/O1000\r\n%"),
                    ("数据在前 + 请求行 + %",
                     "/O1000\r\n%CLOD    PWD     00\r\n%")):
    reply_text(body)
    out = post("/Brother/CNC/PWD", {"connectionId": conn})
    print("=== PWD %s" % label)
    print("  应答体 %r" % body)
    print("  %s" % brief(out))

for label, body in CANDIDATES:
    reply_text(body)
    out = post("/Brother/CNC/GetToolList", {"connectionId": conn})
    print("=== %s" % label)
    print("  应答体 %r" % body)
    print("  %s" % brief(out))

print("=== GetMaintenanceData param=23（PDSP，坐标/进给）")
for label, body in (
        ("X01 13 个值（逗号）",
         "X01,100,2,3,7,5,6,7,8,9,10,100,80,70\r\n00"),
        ("X01 + P01/P02/P03",
         "X01,100,2,3,7,5,6,7,8,9,10,100,80,70\r\n"
         "P01,1.5,2.5,3.5\r\nP02,1.0,2.0,3.0\r\nP03,4.0,5.0,6.0\r\n00"),
        ("C01/T01 键值",
         "C01,10,3,30\r\nT01,3600,86400,7200"),
        ("P03 给 6 个值",
         "P03,4.0,5.0,6.0,7.0,8.0,9.0"),
        ("P03 给 3 个值 + X01",
         "X01,100,2,3,7,5,6,7,8,9,10,100,80,70\r\nP03,4.0,5.0,6.0")):
    reply_text(body)
    out = post("/Brother/CNC/GetMaintenanceData", {"connectionId": conn,
                                                  "param": 23})
    print("=== %s" % label)
    print("  应答体 %r" % body)
    print("  %s" % brief(out))
PY

echo "=== 网关日志尾部"
tail -4 "$work/hp2x.log" || true
