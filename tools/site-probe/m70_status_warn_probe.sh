#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70：状态名（GetStatus）与报警（GetWarning）。
#   GetStatus = getStartStatus + getPauseStatus 两次查询；
#   GetWarning 的应答是 544 字节（GIOP size = 0x214），先试"全 0 = 无报警"。
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

python3 - <<'PY'
import json
import struct
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"


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
        data = json.loads(out)["data"]
    except Exception:                             # noqa: BLE001
        return out[:110]
    if not data.get("success"):
        return "✗ %s" % data.get("error")
    return repr(data.get("value"))


def once(item, spec):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


def frame(value):
    """getStartStatus / getPauseStatus 的 40 字节应答（码 0x1f3c）。"""
    return ("47494f50010001011c000000" "00000000" "3c1f0000" "00000000"
            "000000000300000004000000" + struct.pack("<I", value).hex())


print("=== 状态：getStartStatus(启动中?) 与 getPauseStatus(暂停中?) 各回一帧")
for start, pause in ((1, 0), (0, 0), (0, 1), (1, 1)):
    spec = "SEQ:%s|%s" % (frame(start), frame(pause))
    print("  start=%d pause=%d  %s" % (start, pause, brief(once("GetStatus",
                                                                spec))))

print("=== 报警：544 字节全 0 的应答")
print("  全 0        %s" % brief(once("GetWarning",
                                     "ZREP:544,8:14020000,20:00000000")))
print("=== 文件列表（先看请求长什么样）")
print("  %s" % brief(once("GetFileList", "ZREP:544,8:14020000,20:00000000")))
PY

echo "=== 请求"
grep -A7 -- "--- request" "$work/mock.log" | grep -E 'request|^00[0-3]0 ' || true
