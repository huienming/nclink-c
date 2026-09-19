#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Last black-box idea for the GSK reply: the response is probably the request's
# frame *plus* the value, i.e. one byte longer body (length field +2) with the
# same "00 ... 55 aa" framing. Sweep the two extra bytes with a fresh connection
# per candidate.
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
# request body: 00 6f c8 1e 64 1e 17 10 17 11  (10 bytes)
session = bytes.fromhex("006fc81e641e17101711")


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


def ask(extra, label):
    body = session + extra
    frame = bytes([0x93, 0x00, len(body)]) + body + bytes([0x00, 0x55, 0xaa])
    with open(reply_file, "w") as handle:
        handle.write(frame.hex())
    opened = post("/GSK/CNC/Open/TCP",
                  {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 3})
    conn = (opened.get("data") or {}).get("connectionId")
    if not conn:
        return "open failed"
    out = post("/GSK/CNC/STATUS", {"connectionId": conn}) or {}
    data = out.get("data") or {}
    msg = out.get("message", "")
    if not msg and "value" in data:
        msg = "VALUE=%r" % (data.get("value"),)
    return (msg or str(out))[:70]


seen = {}
# Explore the *shape* instead of single bytes: shorter and longer bodies, with
# the last body byte being the only thing that varies.
request_body = bytes.fromhex("006fc81e641e17101711")
for length in range(1, 15):
    for last in (0x00, 0x01, 0x02, 0x0a, 0x11, 0x81, 0x91, 0xff):
        body = bytearray(request_body[:length])
        if length > len(request_body):
            body.extend(bytes(length - len(request_body)))
        body[0] = 0x00
        body[-1] = last
        frame = bytes([0x93, 0x00, length]) + bytes(body) + bytes([0x00, 0x55, 0xaa])
        with open(reply_file, "w") as handle:
            handle.write(frame.hex())
        opened = post("/GSK/CNC/Open/TCP",
                      {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 3})
        conn = (opened.get("data") or {}).get("connectionId")
        out = post("/GSK/CNC/STATUS", {"connectionId": conn}) if conn else {}
        data = (out or {}).get("data") or {}
        msg = (out or {}).get("message", "")
        if not msg and "value" in data:
            msg = "VALUE=%r" % (data.get("value"),)
        seen.setdefault((msg or str(out))[:70], []).append(
            "len=%d last=%02x" % (length, last))

for msg, cases in sorted(seen.items(), key=lambda kv: len(kv[1])):
    print("%-72s %s" % (msg[:72], " ".join(cases[:8])))
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
