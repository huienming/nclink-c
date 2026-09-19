#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 文件层第二次：找"目录读完"的信号。
#   会话：mochaFSStatFile → mochaFSOpenDirectory → mochaFSReadDirectory（循环）
#   状态码 = 应答 [20..23]，0 = 正常；文件名 = 应答 [40..]。
#   状态 0 时 ReadDirectory 会一直轮询，所以猜"状态非 0 = 读完"。
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

timeout 90 python3 - <<'PY' || echo "（python 这段超时退出了）"
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"

OK = "ZREP:40,7:01,8:1c000000,20:00000000"
END = "ZREP:40,7:01,8:1c000000,20:01000000"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=12).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out):
    try:
        data = json.loads(out)["data"]
    except Exception:                             # noqa: BLE001
        return out[:110]
    if not data.get("success"):
        return "✗ %s" % data.get("error")
    return repr(data.get("value"))


def once(item, spec, extra=None):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    body = {"connectionId": conn}
    body.update(extra or {})
    return post(ROOT + "/" + item, body)


def named(name):
    """带文件名的读目录应答：文件名放 [40..]。"""
    total = 40 + len(name)
    return "ZREP:%d,7:01,8:%s,20:00000000,40:%s" % (
        total, (total - 12).to_bytes(4, "little").hex(), name.encode().hex())


print("=== 状态非 0 当成'目录读完'")
print("  1xOK + END      %s" % brief(once("GetFileList",
                                          "SEQ:%s|%s|%s" % (OK, OK, END))))

print("=== 目录里有一条 O1000")
print("  2xOK + O1000 + END %s" % brief(once(
    "GetFileList", "SEQ:%s|%s|%s|%s" % (OK, OK, named("O1000"), END))))

print("=== 读文件：应答里给 5 字节载荷")
print("  ReadFile        %s" % brief(once(
    "ReadFile", "ZREP:45,7:01,8:21000000,20:00000000,40:48454c4c4f",
    {"fileName": "O1000"})))
PY

echo "=== 请求（前 30 行）"
grep -A6 -- "--- request" "$work/mock.log" | head -40 || true
