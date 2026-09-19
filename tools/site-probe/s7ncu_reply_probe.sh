#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# S7NCU answers in standard S7comm: a Setup Communication ack, then a Read Var
# ack whose data block carries the value. Build both by hand (03 册 documents the
# layout) and put a recognisable number in the data: whatever comes back tells us
# where the gateway reads it from.
#
#   /hp2x    read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 102 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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


def s7_frame(pdu_ref, payload):
    """TPKT + COTP + S7 header (Ack_Data) around a payload (param+data)."""
    total = 4 + 3 + 10 + len(payload)
    head = bytes([0x03, 0x00, total >> 8, total & 0xFF,  # TPKT
                  0x02, 0xF0, 0x80,                      # COTP Data
                  0x32, 0x03, 0x00, 0x00,                # S7 Ack_Data
                  pdu_ref >> 8, pdu_ref & 0xFF])
    return head + payload


def read_ack(value_bytes, pdu_ref=1):
    # parameters: Ack_Data, 1 item, success, transfer size, bit length
    param = bytes([0x04, 0x01, 0xFF, 0x04, 0x00, len(value_bytes) * 8])
    # data item: return code 0xFF = success, transport size, bit length, fill
    data = bytes([0xFF, 0x04, 0x00, len(value_bytes) * 8])
    if len(value_bytes) % 2:
        data += b"\x00"
    data += value_bytes
    header = bytes([0x32, 0x03, 0x00, 0x00,
                    pdu_ref >> 8, pdu_ref & 0xFF,
                    len(param) >> 8, len(param) & 0xFF,
                    len(data) >> 8, len(data) & 0xFF])
    body = header + param + data
    total = 4 + 3 + len(body)
    return (bytes([0x03, 0x00, total >> 8, total & 0xFF, 0x02, 0xF0, 0x80]) +
            body)


opened = json.loads(post("/S7NCU/Open/TCP",
                         {"ipAddress": "127.0.0.1", "port": 102, "timeout": 3}))
conn = (opened.get("data") or {}).get("connectionId")
print("connectionId = %s" % conn)


def ask(value_bytes, label, item="/S7NCU/PartCount"):
    # The mock plays a minimal S7 server, echoing the PDU reference it saw.
    with open(reply_file, "w") as handle:
        handle.write("S7S:" + value_bytes.hex())
    out = post(item, {"connectionId": conn})
    print("--- %-28s %s" % (label, out[:200]))


def ask_raw(value_bytes, label, item="/S7NCU/PartCount"):
    """Data section is exactly these bytes: the module reverses the first eight
    of them into a float64, so feeding reverse(double) should return it."""
    with open(reply_file, "w") as handle:
        handle.write("S7R:" + value_bytes.hex())
    out = post(item, {"connectionId": conn})
    print("--- %-28s %s" % (label, out[:200]))


ask(bytes([0x12, 0x34]), "value = 0x1234")
ask(bytes([0x00, 0x2A]), "value = 42")
# The module wanted at least 68 bytes of data (its own panic said so): send that
# many with distinct values and see which one shows up in the answer.
ask(bytes.fromhex("4045000000000000"), "double 42.0 (8 bytes)")
ask(bytes.fromhex("000000000000002A"), "int64 42 (BE, 8 bytes)")
ask(bytes.fromhex("0000000000000002"), "int64 2 (BE, 8 bytes)")
ask(bytes(range(1, 69)), "68 bytes, values 1..68")
ask_raw(bytes.fromhex("4045000000000000"), "raw: bits of 42.0")
ask_raw(bytes.fromhex("0000000000004540"), "raw: bits of 42.0 (LE order)")

import struct


def raw_for(value):
    """The data section's first eight bytes are the value as a little-endian
    float64 — verified: sending pack('<d', 42.0) came back as 42."""
    return struct.pack("<d", value)


ask_raw(raw_for(1234.5), "raw: 1234.5")
ask_raw(raw_for(41.5), "raw: 41.5")

# FeedActual reads a double somewhere past the start: put 42.0 at a few offsets
# and see which one comes back.
for offset in (0, 8, 16, 24, 32, 40, 48, 56, 64):
    data = bytearray(128)
    data[offset:offset + 8] = raw_for(42.0)
    ask_raw(bytes(data), "FeedActual: 42.0 at %d" % offset, "/S7NCU/FeedActual")

# Execution and Mode ask for two items: answer with one and two items of a few
# lengths (S7S = item headers + markers, one per requested item).
for item in ("Execution", "Mode"):
    for n in (2, 4, 8, 16):
        ask(bytes(range(0x10, 0x10 + n)), "%s: %d byte items" % (item, n),
            "/S7NCU/" + item)

print()
print("=== what the gateway sent (last 1000 bytes of the mock log) ===")
print(open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()[-1000:])
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
