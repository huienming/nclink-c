#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 新代 SYNTEC：应答形状（反汇编 (*SyntecCnc).RRegister 0x64d6b4 + GetResponse 0x64d1e0）
#
#   GetResponse(conn, frame, len, cap, **offset = 20**) → 应答 [20:] 才是"正文"；
#   偏移超过应答长度就报 `error response length`（这就是我们之前看到的那句）。
#   RRegister 再对正文 binary.Read 一个 u16 → 寄存器值。
#   所以最小应答 = 请求回声 + [20..21] = 寄存器值。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 8000,5566-5572 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import re
import struct
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/SYNTEC/CNC"


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
        match = re.search(r"(error response[^\\\"]*|short read[^\\\"]*|"
                          r"panic[^\\\"]*|exception recovered: [^\\\"]*)", error)
        return "✗ %s" % (match.group(0) if match else error[-90:])
    return repr(data.get("value"))


def once(item, spec):
    opened = post(ROOT + "/Open/TCP", {"ipAddress": "127.0.0.1", "port": 8000,
                                       "timeout": 3})
    try:
        conn = (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        conn = None
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


def echo(value, cut=None):
    """回声请求 + [20..21] = u16 值。"""
    spec = "EREP:20:%s" % struct.pack("<H", value).hex()
    if cut:
        spec += ",cut:%d" % cut
    return spec


print("=== PART_COUNT（寄存器 1000，u16 在应答 [20..21]）")
for value in (1234, 7, 0xFFFF):
    print("  %-6d %s" % (value, brief(once("PART_COUNT", echo(value)))))

print("=== PART_COUNT：应答只回 20 字节（正文长度 0）")
print("  %s" % brief(once("PART_COUNT", echo(1234, cut=20))))

print("=== STATUS（NcStateGetValue，值 0..5 的枚举）")
for value in range(0, 6):
    print("  %-3d %s" % (value, brief(once("STATUS", echo(value)))))

print("=== LINE_NUMBER / FEED_SPEED / SPDL_SPEED / 倍率")
for item in ("LINE_NUMBER", "FEED_SPEED", "SPDL_SPEED", "FEED_OVERRIDE",
             "SPDL_OVERRIDE"):
    print("  %-16s %s" % (item, brief(once(item, echo(4321)))))

print("=== FEED_SPEED：它连问三次（寄存器 700、状态 12、状态 76）")
# getFeedSpeed：v1 = RRegister(700)、v2 = NcStateGetValue(12)、v3 = NcStateGetValue(76)
#   v3 == 70 → 直接 float64(v1)；否则 v2 走单位换算表
direct = "SEQ:%s|%s|%s" % (echo(4321), echo(0), echo(70))
print("  v3=70（直通）      %s" % brief(once("FEED_SPEED", direct)))
print("  v3=70, v1=1234     %s" % brief(once(
    "FEED_SPEED", "SEQ:%s|%s|%s" % (echo(1234), echo(0), echo(70)))))
units = "SEQ:%s|%s|%s" % (echo(4321), echo(0), echo(1))
print("  v3=1（单位换算）   %s" % brief(once("FEED_SPEED", units)))
units2 = "SEQ:%s|%s|%s" % (echo(4321), echo(32), echo(1))
print("  v2=32（换算表次档）%s" % brief(once("FEED_SPEED", units2)))
print("  ---- 用 mock.py 的 MAP: 按请求里的寄存器号分别回")
# 请求 [28..29] = 寄存器号：700 = bc02、12 = 0c00、76 = 4c00
reg_map = ("MAP:28:2:bc02=%s|0c00=%s|4c00=%s|%s"
           % (echo(4321), echo(0), echo(70), echo(0)))
print("  700=4321, 12=0, 76=70 %s" % brief(once("FEED_SPEED", reg_map)))
reg_map2 = ("MAP:28:2:bc02=%s|0c00=%s|4c00=%s|%s"
            % (echo(1234), echo(0), echo(70), echo(0)))
print("  700=1234, 12=0, 76=70 %s" % brief(once("FEED_SPEED", reg_map2)))
reg_map3 = ("MAP:28:2:bc02=%s|0c00=%s|4c00=%s|%s"
            % (echo(4321), echo(0), echo(1), echo(0)))
print("  700=4321, 12=0, 76=1  %s" % brief(once("FEED_SPEED", reg_map3)))
print("  ---- 上面这几次在设备侧长什么样（连接/请求数）")
log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
print("       connect 次数 = %d，request 次数 = %d"
      % (log.count("--- connect from"), log.count("--- request")))
for block in log.split("--- request")[-6:]:
    line = block.split("\n")
    if len(line) > 1:
        print("       %s | %s" % (line[0].strip(),
                                  line[1][6:53].replace("  ", " ").strip()))

print("=== PROGRAM / WARNING：先用同一个回声试试")
print("  PROGRAM        %s" % brief(once("PROGRAM", echo(0x4F31))))
print("  PROGRAM(文本)  %s" % brief(once("PROGRAM", "EREP:20:4f31303030")))
print("  WARNING        %s" % brief(once("WARNING", echo(0))))
PY

echo "=== 请求（前 6 帧）"
grep -A8 -- "--- request" "$work/mock.log" | head -40 || true
