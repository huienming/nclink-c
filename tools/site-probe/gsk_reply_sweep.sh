#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Find the GSK reply's "command" byte by brute force: keep one connection open,
# answer every request with a frame whose command byte is the candidate, and let
# the gateway itself tell us whether it accepted it ("unexpected response
# command" means no, anything else means we moved the parser forward).
#
#   /hp2x    read-only mount of .../app1/hp2x
#   /work    this directory
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

# The mock re-reads this file for every request.
printf '' >"$work/reply.txt"
python3 /work/mock.py 6000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - <<'PY'
import json, os, time, urllib.request

base = "http://127.0.0.1:33123"
reply_file = "/tmp/run/reply.txt"

# STATUS request as captured from the gateway, with the command byte at offset 12
# and a "checksum-ish" byte at 13:  93 00 0a 00 6f c8 1e 64 1e 17 10 17 11 00 55 aa
head = bytes([0x93, 0x00, 0x0a, 0x00, 0x6f, 0xc8, 0x1e, 0x64,
              0x1e, 0x17, 0x10, 0x17])
tail = bytes([0x00, 0x55, 0xaa])


def post(path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return json.loads(urllib.request.urlopen(req, timeout=10).read())
    except Exception as exc:                  # noqa: BLE001
        return {"code": -1, "message": str(exc)}


opened = post("/GSK/CNC/Open/TCP",
              {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 3})
conn = (opened.get("data") or {}).get("connectionId")
print("connectionId = %s" % conn)
if not conn:
    raise SystemExit("open failed: %s" % opened)

request = bytes.fromhex("93000a006fc81e641e171017110055aa")


def ask(frame):
    with open(reply_file, "w") as handle:
        handle.write(frame.hex())
    out = post("/GSK/CNC/STATUS", {"connectionId": conn}) or {}
    data = out.get("data") or {}
    return out.get("message", "") or ("value=%r" % (data.get("value"),))


baseline = ask(request)               # echo the request: known to be rejected
print("baseline: %s" % baseline[:60])

# Vary one byte at a time: whichever position the parser actually checks will
# change the message for at least one value.
hits = {}
for pos in range(len(request)):
    for val in range(0x00, 0x100):
        frame = bytearray(request)
        frame[pos] = val
        msg = ask(bytes(frame))
        if msg != baseline:
            hits.setdefault(pos, []).append((val, msg))
    if pos in hits:
        print("pos %2d matters: %s" % (
            pos, " ".join("%02x->%s" % (v, m[:34]) for v, m in hits[pos][:6])))
    else:
        print("pos %2d: no effect" % pos)
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
