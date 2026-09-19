#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 PLC 第五轮：结束码（PLC 报错）会不会被驱动吃掉、子头是不是根本不看、
# 以及"应答数据比要的少"时的边界（上一轮出现过 HTTP 500）。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 6000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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
        return urllib.request.urlopen(req, timeout=15).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out, limit=6):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-80:]
    value = data.get("value") if "value" in data else data
    if isinstance(value, list):
        return "%d 项 %s" % (len(value), value[:limit])
    return repr(value)


def set_reply(text):
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(text)


def read(device, index, size, reply, limit=6):
    opened = post("/Mitsubishi/Plc/MC/Open/TCP", {"ipAddress": "127.0.0.1",
                                                 "port": 6000, "timeout": 3})
    try:
        conn = (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        conn = None
    set_reply(reply)
    return brief(post("/Mitsubishi/Plc/MC/Read",
                      {"connectionId": conn, "deviceType": device,
                       "index": index, "size": size}), limit)


print("=== ① PLC 报错（结束码 0xC050 = 地址越界）")
print("  只有结束码（长度=2）      %s"
      % read("D", 100, 4, "d00000ffff0300" + "0200" + "c050"))
print("  结束码 + 2 字节数据      %s"
      % read("D", 100, 4, "d00000ffff0300" + "0400" + "c050" + "1100"))
print("  结束码 0xC051 长度=2      %s"
      % read("D", 100, 4, "d00000ffff0300" + "0200" + "c051"))

print("=== ② 子头（d0/d4/任意）驱动看不看")
print("  子头 d4（4E 样子）        %s"
      % read("D", 100, 1, "d40000ffff0300" + "0400" + "0000" + "1100"))
print("  子头 ff                 %s"
      % read("D", 100, 1, "ff0000ffff0300" + "0400" + "0000" + "1100"))
print("  子头 00                 %s"
      % read("D", 100, 1, "000000ffff0300" + "0400" + "0000" + "1100"))

print("=== ③ 应答数据比要的少（形状自洽，但不够长）")
print("  要 4 字、只给 3 字       %s"
      % read("D", 100, 4, "d00000ffff0300" + "0800" + "0000" + "110022003300"))
print("  要 8 点、只给 1 字节     %s"
      % read("M", 100, 8, "d00000ffff0300" + "0300" + "0000" + "01"))

print("=== ④ 点数很大的时候（要 2000 字，PLC 通常会报 0x0006）")
print("  给足数据                 %s"
      % read("D", 0, 2000, "d00000ffff0300" + "a20f" + "0000" + "11" * 4000,
             limit=3))
PY

echo "=== 网关日志尾部"
tail -4 "$work/hp2x.log" || true
