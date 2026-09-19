#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# The device protocols live in the Go gateway (hp2x_box200), not in the C++
# plugins: the plugins POST /<module>/<device>/<item> to it. So run the gateway
# itself and point its target at our fake machine — the gateway is a static Go
# binary, so none of the C++ ABI trouble applies.
#
#   /hp2x    read-only mount of .../app1/hp2x
#   /work    this directory
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

# $1 (optional) is the hex reply the fake machine sends to every frame.
python3 /work/mock.py 6000 "${1:-}" >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

echo "== starting hp2x_box200 (the device protocol gateway)"
./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - "$2" <<'PY'
import json, sys, urllib.request

base = "http://127.0.0.1:33123"
items = ["/GSK/CNC/STATUS", "/GSK/CNC/PART_COUNT", "/GSK/CNC/PROGRAM",
         "/GSK/CNC/LINE_NUMBER", "/GSK/CNC/FEED_SPEED", "/GSK/CNC/SPDL_SPEED",
         "/GSK/CNC/FEED_OVERRIDE", "/GSK/CNC/SPDL_OVERRIDE",
         "/GSK/CNC/RAPID_OVERRIDE", "/GSK/CNC/TOOL_NUMBER", "/GSK/CNC/WARNING"]


def post(path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        out = urllib.request.urlopen(req, timeout=15).read().decode("utf-8", "replace")
    except Exception as exc:                  # noqa: BLE001
        print("--- POST %-26s ERR %s" % (path, exc))
        return None
    print("--- POST %-26s %s" % (path, out[:200]))
    try:
        return json.loads(out)
    except Exception:                         # noqa: BLE001
        return None


def frames():
    """How many device-side frames the fake machine has seen so far."""
    try:
        text = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    except OSError:
        return 0
    return text.count("--- request")


opened = post("/GSK/CNC/Open/TCP",
              {"ipAddress": "127.0.0.1", "port": 6000, "timeout": 5})
conn = None
if opened and isinstance(opened.get("data"), dict):
    conn = opened["data"].get("connectionId")
print("   connectionId = %s (frames so far %d)" % (conn, frames()))

if conn:
    for item in items:
        before = frames()
        post(item, {"connectionId": conn})
        print("   -> %s: %d frame(s)" % (item, frames() - before))
PY

sleep 1
kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true

echo
echo "===== what the gateway sent to the fake machine ====="
cat "$work/mock.log"
echo
echo "===== gateway log (tail) ====="
tail -20 "$work/hp2x.log" 2>/dev/null || echo "(no log)"
