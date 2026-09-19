#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 北京精雕第四轮：把完整错误串打出来，看它到底还在等多少字节。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 7080-7090 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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
ROOT = "/JINGDIAO/CNC"


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


def once(item, spec):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 7080,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


def tail(out):
    """只留错误里最后那段中文/英文可读部分。"""
    text = out.replace("\\u0000", "")
    match = re.search(r"short read[^\"]*", text)
    if match:
        return match.group(0)
    match = re.search(r"packet type error: \d+", text)
    if match:
        return match.group(0)
    match = re.search(r"exception recovered: [^\"]*", text)
    if match:
        return match.group(0)
    return text[-140:]


def frame(second):
    total = 24 + len(second)
    return "ZREP:%d,2:02,3:01,24:%s" % (total, second.hex())


PATTERN = bytes(range(24))
for count in (1, 2, 3, 4):
    body = (PATTERN * count)[:24 * count]
    print("  第2包起 %d 包  长度 %-4d %s" % (count, 24 + 24 * count,
                                            tail(once("PART_COUNT",
                                                      frame(body)))))

print("  ---- 只有头（24 字节）")
print("  %s" % tail(once("PART_COUNT", "ZREP:24,2:02,3:01")))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -12 || true
