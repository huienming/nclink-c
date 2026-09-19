#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 文件层（GetFileList → mochaFSOpenDirectory / mochaFSReadDirectory / …）。
#
# mochaFSReadDirectory 的应答解析（反汇编 0x63a124 起）：
#   长度必须 ≥ 24，状态码 = [20..23] 的 u32（非 0 = 出错），
#   文件名 = 应答 [40..]，结束条件应是"载荷为空"（长度正好 40）。
# 所以先回"40 字节、状态 0、无载荷"的帧看看能不能收口。
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

timeout 60 python3 - <<'PY' || echo "（python 这段超时退出了）"
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=8).read().decode("utf-8",
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


print("=== 40 字节、状态 0、无载荷的应答（文件列表）")
print("  GetFileList %s" % brief(once("GetFileList",
                                     "ZREP:40,7:01,8:1c000000,20:00000000")))

print("=== 40 字节、状态 0 的应答（读文件，看会不会开口要数据）")
print("  ReadFile    %s" % brief(once("ReadFile",
                                     "ZREP:40,7:01,8:1c000000,20:00000000",
                                     {"fileName": "O1000"})))

print("=== 状态码非 0（=1）看错误路径")
print("  GetFileList %s" % brief(once("GetFileList",
                                     "ZREP:40,7:01,8:1c000000,20:01000000")))
PY

echo "=== 前 40 行请求"
grep -A6 -- "--- request" "$work/mock.log" | head -60 || true
