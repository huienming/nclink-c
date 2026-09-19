#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Brother replies, probed the same way as KEDE: send a candidate answer to one
# item and let the gateway say what it thinks (its errors name what it wanted).
#
#   /hp2x    read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 10000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - <<'PY'
import json, urllib.error, urllib.request

base = "http://127.0.0.1:33123"
reply_file = "/tmp/run/reply.txt"


def post(path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=10).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                  # noqa: BLE001
        return "ERR %s" % exc


opened = json.loads(post("/Brother/CNC/Open/TCP",
                         {"ipAddress": "127.0.0.1", "port": 10000, "timeout": 3}))
conn = (opened.get("data") or {}).get("connectionId")
print("connectionId = %s" % conn)


def ask(xml, label, item="/Brother/CNC/PWD"):
    with open(reply_file, "w") as handle:
        handle.write("TEXT:" + xml + "\n")
    out = post(item, {"connectionId": conn})
    print("--- %-44s %s" % (label, out[:200]))


candidates = [
    "%CFLDPWD01%",
    "%CFLDPWD 01%",
    "%CFLDPWD /user/path 01%",
    "%CLOD/user/path01%",
    "/user/path",
    "01%/user/path",
    "%/user/path%",
]
for xml in candidates:
    ask(xml, xml[:40])
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
