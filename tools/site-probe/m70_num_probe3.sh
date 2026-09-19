#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 第三批：开机/累计/加工时间（getTime）+ 状态（getStartStatus/getPauseStatus）。
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


def int4(value):
    """尾巴 + [36..39] 的 int32/uint32。"""
    return ("EREP:7:01,8:1c000000,20:00000000,"
            "24:0000000003000000 04000000,36:%s,cut:40"
            % struct.pack("<i", value).hex()).replace(" ", "")


print("=== 时间（getTime，三项共用模板，码 0x62b8）")
for item in ("GetTimePowerOn", "GetTimeCumulative", "GetTimeMachining"):
    print("  %-20s %s" % (item, brief(once(item, int4(3600)))))
for value in (0, 1, 65535, 12345678, -1):
    print("  %-20s %s" % ("Machining=%d" % value,
                          brief(once("GetTimeMachining", int4(value)))))

print("=== 状态（GetStatus = getStartStatus + getPauseStatus 两次查询）")
for value in (0, 1, 2):
    print("  两项都回 %d      %s" % (value, brief(once("GetStatus", int4(value)))))
PY

echo "=== getTime / 状态的请求"
grep -A7 -- "--- request" "$work/mock.log" | grep -E 'request|^00[0-4]0 ' || true
