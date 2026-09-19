#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Execution 专用探针：现在知道它的请求是"两项 SZL 子项"，应答也必须回两项。
# 这个脚本一边扫形状，一边把**两边的日志**都留下来：
#   * mock.log —— 网关实际发出去的请求（含 39 字节的 Execution 读帧）
#   * hp2x.log —— 网关自己怎么描述这次失败
# 光看 "error response" 是分不出原因的，得看它自己说什么。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf 'S7S:41000000,41000000' >"$work/reply.txt"
python3 /work/mock.py 102 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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


def once(spec, path="/S7NCU/Execution"):
    opened = json.loads(post("/S7NCU/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 102,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(path, {"connectionId": conn})


import struct


# Mode 反汇编读出来是一张真表（值取自每一项的低 16 位）：
#   (0,1)="REPOS"  (0,3)="REFPOINT"  (0,0)="JOG"  第一项==2 → "AUTO"，其余 → "AUTO"
# 但我们喂 4 字节数据时**永远是 AUTO**，怀疑 ResolveRes 给第二项的切片带上了
# 4 字节数据项头（于是低 16 位恒为 0x04FF）。所以扫一遍两项的"声明长度"。
def mode_cell(items):
    out = once("S7S:" + ",".join(items), "/S7NCU/Mode")
    try:
        data = json.loads(out)["data"]
    except Exception:                             # noqa: BLE001
        return "?"
    return repr(data.get("value")) if data.get("success") else "err"


print("=== Mode：第二项长度 1..20（第一项固定 4 字节 0），找 JOG/REPOS/REFPOINT")
for length in range(1, 21):
    print("  第二项 %-3d 字节 -> %s" % (length, mode_cell(["00000000", "00" * length])))

print("=== Mode：第一项长度 1..20（第二项固定 4 字节 0）")
for length in range(1, 21):
    print("  第一项 %-3d 字节 -> %s" % (length, mode_cell(["00" * length, "00000000"])))

print("=== Mode：4 字节码按反汇编表逐个试（两项各自独立）")
for code, name in [(0, "JOG"), (2, "AUTO")]:
    a = struct.pack("<I", code).hex()
    print("  两项都是 %#x -> %s" % (code, mode_cell([a, a])))
PY

echo "=== 网关发给机床的 Execution 读帧（mock.log 里最后一个 39 字节请求）"
python3 - <<'PY'
import re

log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
blocks = re.findall(r"--- request \d+ bytes\n((?:[0-9a-f]{4}  .*\n)+)", log)
frames = []
for block in blocks:
    raw = bytearray()
    for line in block.rstrip("\n").split("\n"):
        for token in line[6:53].split():
            raw.append(int(token, 16))
    if len(raw) >= 4 and raw[:4] == b"\x03\x00\x00\x27":
        frames.append(bytes(raw))
print("共 %d 帧 39 字节的读请求；最后一帧：" % len(frames))
if frames:
    raw = frames[-1]
    for i in range(0, len(raw), 16):
        chunk = raw[i:i + 16]
        print("  %04x  %-47s  %s" % (
            i, " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
PY

echo "=== hp2x.log 尾 40 行（网关自己的说法）"
tail -40 "$work/hp2x.log"
