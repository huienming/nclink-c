#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# JINGDIAO: the request is a 24 byte frame (12 fixed bytes, a 4 byte counter, a
# 2 byte item code). Answer with a frame whose bytes are all different: whatever
# number comes back names the offset the parser took the value from.
#
#   /hp2x    read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 7080-7090 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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
        return json.loads(urllib.request.urlopen(req, timeout=10).read())
    except urllib.error.HTTPError as exc:
        return {"message": "HTTP %d %s" % (exc.code,
                                           exc.read().decode("utf-8", "replace"))}
    except Exception as exc:                  # noqa: BLE001
        return {"message": "ERR %s" % exc}


opened = post("/JINGDIAO/CNC/Open/TCP",
              {"ipAddress": "127.0.0.1", "port": 7080, "timeout": 3})
conn = (opened.get("data") or {}).get("connectionId")
print("connectionId = %s" % conn)


def ask(frame_bytes, label, item="/JINGDIAO/CNC/PART_COUNT"):
    with open(reply_file, "w") as handle:
        handle.write(frame_bytes.hex())
    out = post(item, {"connectionId": conn})
    data = (out or {}).get("data") or {}
    print("--- %-40s value=%-8r msg=%s" % (label, data.get("value"),
                                           (out or {}).get("message", "")[:60]))


# The parser said the packet type is at offset 2 and wants 0x02 (its own words:
# "[response(0x02)] packet type error: <what we sent>"). Set that and see what it
# says next.
def with_type(body, ptype=0x02):
    frame = bytearray(body)
    frame[2] = ptype
    return bytes(frame)


ask(with_type(range(1, 25)), "type=2, rest 1..24")
ask(with_type(bytes(24)), "type=2, rest zero")
ask(with_type(bytes.fromhex("050000031000000018000000000000000000000000")),
    "type=2, rest of the request")
ask(with_type(bytes.fromhex("050000031000000018000000000000000000000000")[:12] +
              bytes([1, 0, 0, 0]) + bytes([2, 0]) + bytes(6),
              ), "type=2, counter=1 code=2")
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
