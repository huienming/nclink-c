#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 精雕 `GetResponse`（20 册 §2 最后一条）：裸透传 —— 请求体 {connectionId, data}，
# 网关把 data 原样发出去、把设备回来的字节原样带回（跟那智/KUKA 的 GetResponse 同形，
# 现场 Lua 里就是这么用的：`{connectionId = cid, data = "XH\r\n"}`）。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 7080 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
ROOT = "/JINGDIAO/CNC"
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


def brief(out, limit=200):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-120:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data.get("value") if "value" in data else data)[:limit]


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


opened = post(ROOT + "/Open/TCP", {"ipAddress": "127.0.0.1", "port": 7080,
                                   "timeout": 3})
conn = (json.loads(opened).get("data") or {}).get("connectionId")
print("=== %s/Open/TCP connectionId=%s" % (ROOT, conn))

# 先试"裸调"（上一轮就是这样 panic 的），再试"先 Bind 再裸调"
with open(REPLY, "w", encoding="latin-1") as handle:
    handle.write("")
print("=== 先不 Bind 直接调：%s"
      % brief(post(ROOT + "/GetResponse", {"connectionId": conn,
                                           "data": "XH\r\n"})))

with open(REPLY, "w", encoding="latin-1") as handle:
    handle.write("ZREP:84,2:02,3:01,26:02,27:02,56:1c000000,36:07000000")
print("=== Bind：%s" % brief(post(ROOT + "/Bind", {"connectionId": conn})))

for label, data, reply in (
        ("ASCII 文本", "XH\r\n", "TEXT:OK-XH\r\n"),
        ("十六进制串", "1b02aabb", "1b02801f00"),
        ("空数据", "", ""),
        ("长一点", "M1234ABCDEFGH", "TEXT:" + "Z" * 40 + "\n")):
    before = len(requests())
    with open(REPLY, "w", encoding="latin-1") as handle:
        handle.write(reply)
    out = post(ROOT + "/GetResponse", {"connectionId": conn, "data": data})
    chunks = requests()[before:]
    print("=== GetResponse data=%r" % data)
    print("  返回 %s" % brief(out))
    for raw in chunks:
        print("      → %r" % raw)
PY

echo "=== 网关日志尾部"
tail -3 "$work/hp2x.log" || true
