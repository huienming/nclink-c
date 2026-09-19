#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Sweep the reply's command byte with a *fresh connection per candidate*: the
# gateway caches the failure on a connection, so a single-connection sweep only
# ever reports the first verdict (we learned that the hard way).
#
#   /hp2x    read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 6000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - <<'PY'
import json, urllib.error, urllib.request

base = "http://127.0.0.1:33123"
reply_file = "/tmp/run/reply.txt"
request = bytearray.fromhex("93000a006fc81e641e171017110055aa")


def post(path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return json.loads(urllib.request.urlopen(req, timeout=10).read())
    except urllib.error.HTTPError as exc:
        return {"message": "HTTP %d %s" % (exc.code,
                                           exc.read().decode("utf-8", "replace"))}
    except Exception as exc:                  # noqa: BLE001
        return {"message": "ERR %s" % exc}


def try_candidate(pos, val):
    frame = bytearray(request)
    frame[pos] = val
    with open(reply_file, "w") as handle:
        handle.write(bytes(frame).hex())
    opened = post("/GSK/CNC/Open/TCP",
                  {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 3})
    conn = (opened.get("data") or {}).get("connectionId")
    if not conn:
        return "open failed: %s" % opened.get("message")
    out = post("/GSK/CNC/STATUS", {"connectionId": conn})
    data = (out or {}).get("data") or {}
    msg = out.get("message", "")
    if not msg and "value" in data:
        msg = "VALUE=%r" % (data.get("value"),)
    return msg or str(out)[:120]


for pos in (12, 13, 3, 4, 9):
    seen = {}
    for val in range(0x00, 0x100):
        msg = try_candidate(pos, val)
        seen.setdefault(msg[:60], []).append(val)
    print("== pos %d" % pos)
    for msg, vals in sorted(seen.items(), key=lambda kv: len(kv[1])):
        print("   %-58s %s" % (msg, " ".join("%02x" % v for v in vals[:12])))
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
