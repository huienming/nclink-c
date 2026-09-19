#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 两件事：
#   1) Mode：让假机床把数据段写成 4 字节码，逐个码打一遍 /S7NCU/Mode，
#      看网关把每个码翻译成什么文字——这直接给出"模式码 → 模式名"的表；
#   2) Execution：改回 S7S（按请求项数回），一次给两个数据项，试几组形状。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf 'S7R:00000000' >"$work/reply.txt"
python3 /work/mock.py 102 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import struct
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


def set_reply(spec):
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)


def once(path, spec):
    """失败会被网关缓存在连接上，所以每次都要新开一条连接。"""
    opened = json.loads(post("/S7NCU/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 102,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    set_reply(spec)
    return post(path, {"connectionId": conn})


print("=== 单项读（S7R 的 param 只有 1 项，够用）")
set_reply("S7R:" + struct.pack("<d", 3.5).hex())
print("  PartCount %s" % once("/S7NCU/PartCount", "S7R:" +
                              struct.pack("<d", 3.5).hex())[:130])

print("=== Mode：请求里有两个子项，必须用 S7S 回两项")
for code in [0, 1, 2, 3, 4, 5]:
    two = struct.pack("<I", code).hex() + "," + struct.pack("<I", code).hex()
    out = once("/S7NCU/Mode", "S7S:" + two)
    print("  code=%-2d %s" % (code, out[:150]))


def value_of(out):
    try:
        return json.loads(out)["data"].get("value")
    except Exception:                             # noqa: BLE001
        return out[:60]


Q = struct.pack("<I", 0).hex()
print("=== Mode 分项扫描：到底是哪一项在决定返回的模式名")
for code in range(0, 14):
    v = struct.pack("<I", code).hex()
    first = value_of(once("/S7NCU/Mode", "S7S:%s,%s" % (v, Q)))
    second = value_of(once("/S7NCU/Mode", "S7S:%s,%s" % (Q, v)))
    both = value_of(once("/S7NCU/Mode", "S7S:%s,%s" % (v, v)))
    print("  code=%-3d 第一项=%-8s 第二项=%-8s 两项=%s" %
          (code, first, second, both))

print("=== Mode 文本扫描（是不是把数据当字符串读）")
for text in [b"JOG", b"MDI", b"AUTO", b"TEACH", b"REPOS", b"REF",
             b"0", b"1", b"2", b"3", b"jog", b"AUTO\x00", b"     "]:
    raw = (text + b"\x00" * 4)[:4]
    v = raw.hex()
    print("  %-8s 两项=%s" % (raw.decode("latin-1"), value_of(
        once("/S7NCU/Mode", "S7S:%s,%s" % (v, v)))))

print("=== Mode 大小端 / 单双字节")
for raw in ["00000000", "00000001", "00000002", "01000000", "02000000",
            "0000000000", "000000000000", "0000000000000000"]:
    print("  %-18s %s" % (raw, value_of(once("/S7NCU/Mode",
                                             "S7S:%s,%s" % (raw, raw)))))

print("=== Execution：两个子项的形状扫描")
sizes = [1, 2, 3, 4, 5, 8, 12, 16]
for first_size in sizes:
    for second_size in sizes:
        items = [bytes([0x41]) * first_size, bytes([0x41]) * second_size]
        out = once("/S7NCU/Execution", "S7S:" + ",".join(i.hex() for i in items))
        if "success\":true" in out:
            print("  ✅ 1+1 形状 %d+%d -> %s" % (first_size, second_size, out[:150]))
            break
    else:
        continue
    break
print("  （没打印 ✅ 就是所有 64 组 41 填充都不被接受）")
PY
