#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 新代 SYNTEC：先在假机床上把设备侧请求抓下来（10 册说帧格式"尚未逆向"）。
#   网关有 /SYNTEC/CNC/* 12 条路由，但 Go 二进制的协议包里没有 syntec ——
#   先确认这条路走不走得通、报文长什么样。
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
        match = re.search(r"(request error[^\\\"]*|short read[^\\\"]*|"
                          r"unexpected[^\\\"]*|exception recovered: [^\\\"]*)",
                          error)
        return "✗ %s" % (match.group(0) if match else error[-100:])
    return repr(data.get("value") if "value" in data else data)


print("=== Open/TCP")
opened = post(ROOT + "/Open/TCP", {"ipAddress": "127.0.0.1", "port": 8000,
                                   "timeout": 3})
print("  %s" % brief(opened))
with open(REPLY, "w", encoding="utf-8") as handle:
    handle.write("")
conn = None
try:
    conn = (json.loads(opened).get("data") or {}).get("connectionId")
except Exception:                                 # noqa: BLE001
    pass

print("=== 逐项")
for item in ("STATUS", "PART_COUNT", "LINE_NUMBER", "PROGRAM", "FEED_SPEED",
             "SPDL_SPEED", "FEED_OVERRIDE", "SPDL_OVERRIDE", "WARNING"):
    print("  %-16s %s" % (item, brief(post(ROOT + "/" + item,
                                           {"connectionId": conn}))))
    # 每项自己开一条连接，所以紧接着把 mock 日志里新增的请求整帧打出来便于逐项对号
    tail = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    block = tail.split("--- request")[-1].split("\n")
    raw = "".join(line[6:53].replace(" ", "")
                  for line in block[1:4] if line.strip())
    print("      %s" % raw)
PY

echo "=== 设备侧请求"
grep -A8 -- "--- request" "$work/mock.log" | head -60 || true
echo "=== 网关日志尾部"
tail -5 "$work/hp2x.log" || true
