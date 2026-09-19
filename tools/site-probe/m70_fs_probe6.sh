#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 文件层第六次：把 mochaFSReadFile（读文件内容）的载荷位置钉死。
#
# 反汇编 0x63a3b8 得：
#   应答 [20..23]（LE u32）非 0 -> 打 "read error, replyStatus of
#                                  mochaFSReadFile: %v\n" 后收摊（0x63a834）
#   应答长度 == 32            -> 打 "read finish" 后收摊（0x63a7bc）= 读完
#   否则 块长 = 应答 [28..31]（LE u32），打 "read size %v\n"，
#        内容 = 应答 [32 .. 32+块长)，返回 (块长, 内容)；
#        长度 < 32 或 < 32+块长 -> panicSliceAcap/panicSliceB
#   ReadFile（0x63b684）就是"块长 != 0 就接着读"的循环，最后拼成一个字符串。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf 'EREP:7:01' >"$work/reply.txt"
python3 /work/mock.py 683 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

timeout 120 python3 - <<'PY' || echo "（python 这段超时退出了）"
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"
NAME = "6d6f63686146535265616446696c65"          # mochaFSReadFile


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=12).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8",
                                                           "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out):
    try:
        data = json.loads(out)["data"]
    except Exception:                             # noqa: BLE001
        return out[:120]
    if not data.get("success"):
        return "✗ %s" % data.get("error")
    return repr(data.get("value"))


def once(spec):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/ReadFile",
                {"connectionId": conn, "fileName": "O1000"})


def finish():
    """32 字节、状态 0：驱动打 read finish 后收摊。"""
    return "ZREP:32,7:01,8:14000000,20:00000000"


def chunk(data):
    """块长放 [28..31]，内容放 [32..]。"""
    raw = data.encode()
    size = 32 + len(raw)
    return "ZREP:%d,7:01,8:%s,20:00000000,28:%s,32:%s" % (
        size, (size - 12).to_bytes(4, "little").hex(),
        len(raw).to_bytes(4, "little").hex(), raw.hex())


OK = "ZREP:32,7:01,8:14000000,20:00000000"


def loopq(parts, filler=None):
    return "LOOPQ:%s/%s/%s" % (NAME, filler or OK, "|".join(parts))


print("=== ① 一块 HELLO + 一帧 read finish")
print("  ReadFile %s" % brief(once(loopq([chunk("HELLO"), finish()]))))

print("=== ② 两块 HEL + LO + read finish（前面的 2 格给①用掉的序号垫掉）")
print("  ReadFile %s" % brief(once(loopq(
    [OK, OK, chunk("HEL"), chunk("LO"), finish()]))))
PY

echo "=== 网关日志（read size / read finish / replyStatus）"
grep -n "read size\|read finish\|replyStatus" "$work/hp2x.log" | head -20 || true
