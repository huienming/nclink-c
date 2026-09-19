#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# `cmd` builds two 8-byte patterns and hands both to shouldResponseCmd:
#   request  : 65 C8 0C 64 0C <sn> 00 00
#   response : 65 64 0C C8 0C <sn> 00 00
# i.e. the reply looks like the request with the two 16-bit words after the
# first byte swapped. Try that (and a few neighbours) on the captured STATUS
# request payload.
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
# captured STATUS request payload (10 bytes)
req = bytearray.fromhex("6fc81e641e1710171100")


def post(path, body):
    r = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                               headers={"Content-Type": "application/json"})
    try:
        return json.loads(urllib.request.urlopen(r, timeout=10).read())
    except urllib.error.HTTPError as exc:
        return {"message": "HTTP %d" % exc.code}
    except Exception as exc:                  # noqa: BLE001
        return {"message": "ERR %s" % exc}


def ask(payload, label):
    frame = (bytes([0x93, 0x00, len(payload), 0x00]) + bytes(payload) +
             bytes([0x55, 0xaa]))
    with open(reply_file, "w") as handle:
        handle.write(frame.hex())
    opened = post("/GSK/CNC/Open/TCP",
                  {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 3})
    conn = (opened.get("data") or {}).get("connectionId")
    out = post("/GSK/CNC/STATUS", {"connectionId": conn}) if conn else {}
    data = (out or {}).get("data") or {}
    print("%-28s value=%-10r msg=%s" % (label, data.get("value"),
                                        (out or {}).get("message", "")[:40]))


ask(req, "echo")

v = bytearray(req)
v[1:5], v[5:9] = req[5:9], req[1:5]          # swap the two 4-byte words
ask(v, "swap words")

v = bytearray(req)
v[1:3], v[3:5] = req[3:5], req[1:3]          # swap 16-bit halves of word0
ask(v, "swap halves w0")

v = bytearray(req)
v[5:7], v[7:9] = req[7:9], req[5:7]          # swap 16-bit halves of word1
ask(v, "swap halves w1")

v = bytearray(req)
v[1:3], v[3:5] = req[3:5], req[1:3]
v[5:7], v[7:9] = req[7:9], req[5:7]
ask(v, "swap both")

for extra in (b"", b"\x00", b"\x00\x00", b"\x01\x00\x00\x00"):
    payload = bytes(req) + extra
    ask(payload, "echo + %d value byte(s)" % len(extra))
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
