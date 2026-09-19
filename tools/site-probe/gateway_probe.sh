#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# The device protocols live in the Go gateway (hp2x_box200), not in the C++
# plugins: the plugins POST /<module>/<device>/<item> to it. So run the gateway
# itself and point its target at our fake machine. The gateway is a static Go
# binary, so none of the C++ ABI trouble applies.
#
#   /hp2x    read-only mount of .../app1/hp2x
#   /work    this directory
#
# Everything comes from the environment - NCL_REPLY (fake machine reply, hex or
# TEXT:...), NCL_OPEN (open path), NCL_ITEMS (comma separated item paths),
# NCL_DEV_PORT (device port, a list or a range) - which avoids shell-quoting
# games. Positional arguments still work: reply, open, items, port.
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

reply=${NCL_REPLY:-${1:-}}
[ "$reply" = "-" ] && reply=""
open_path=${NCL_OPEN:-${2:-/GSK/CNC/Open/TCP}}
dev_port=${NCL_DEV_PORT:-${4:-6000}}
items=${NCL_ITEMS:-${3:-}}
[ -z "$items" ] && items="/GSK/CNC/STATUS,/GSK/CNC/PART_COUNT,/GSK/CNC/PROGRAM,/GSK/CNC/LINE_NUMBER,/GSK/CNC/FEED_SPEED,/GSK/CNC/SPDL_SPEED,/GSK/CNC/FEED_OVERRIDE,/GSK/CNC/SPDL_OVERRIDE,/GSK/CNC/RAPID_OVERRIDE,/GSK/CNC/TOOL_NUMBER,/GSK/CNC/WARNING"
echo "== open=$open_path device-port=$dev_port"

python3 /work/mock.py "$dev_port" "$reply" >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

echo "== starting hp2x_box200 (the device protocol gateway)"
./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - "$open_path" "$items" "$dev_port" <<'PY'
import json, sys, urllib.request

base = "http://127.0.0.1:33123"
open_path = sys.argv[1]
items = [p for p in sys.argv[2].split(",") if p]
dev_port = int(sys.argv[3].split(",")[0].split("-")[0])


def post(path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        out = urllib.request.urlopen(req, timeout=15).read().decode("utf-8",
                                                                    "replace")
    except Exception as exc:                  # noqa: BLE001
        print("--- POST %-34s ERR %s" % (path, exc))
        return None
    print("--- POST %-34s %s" % (path, out[:160]))
    try:
        return json.loads(out)
    except Exception:                         # noqa: BLE001
        return None


def frames():
    """How many device-side frames the fake machine has seen so far."""
    try:
        text = open("/tmp/run/mock.log", encoding="utf-8",
                    errors="replace").read()
    except OSError:
        return 0
    return text.count("--- request")


opened = post(open_path,
              {"ipAddress": "127.0.0.1", "port": dev_port, "timeout": 5})
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
tail -12 "$work/hp2x.log" 2>/dev/null || echo "(no log)"
