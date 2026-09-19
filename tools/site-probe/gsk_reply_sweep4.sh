#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# The reply format, read out of the gateway's own parser (shouldResponseCmd):
#
#   93 00 <len:2 LE> <payload: len bytes> 55 aa        total = len + 6
#   payload = <session 4> <counter 4> <command 1> <value...>
#
# (The earlier sweeps were wrong because they put the payload straight after the
# 0x93 and treated the *high* byte of the length as part of the body.)
# Sweep the command byte with a value appended, as the parser wants.
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
session = bytes.fromhex("6fc81e64")
counter = bytes.fromhex("1e171017")


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


def frame(payload):
    return (bytes([0x93, 0x00, len(payload), 0x00]) + payload +
            bytes([0x55, 0xaa]))


def ask(payload):
    with open(reply_file, "w") as handle:
        handle.write(frame(payload).hex())
    opened = post("/GSK/CNC/Open/TCP",
                  {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 3})
    conn = (opened.get("data") or {}).get("connectionId")
    if not conn:
        return "open failed"
    out = post("/GSK/CNC/STATUS", {"connectionId": conn}) or {}
    data = out.get("data") or {}
    msg = out.get("message", "")
    if "value" in data:
        msg = "VALUE=%r %s" % (data.get("value"), msg)
    return (msg or str(out))[:70]


seen = {}
for cmd in range(0x00, 0x100):
    payload = session + counter + bytes([cmd]) + bytes([0x00, 0x00, 0x00, 0x00])
    msg = ask(payload)
    seen.setdefault(msg, []).append(cmd)

for msg, cmds in sorted(seen.items(), key=lambda kv: len(kv[1])):
    print("%-64s %s" % (msg[:64], " ".join("%02x" % c for c in cmds[:12])))
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
