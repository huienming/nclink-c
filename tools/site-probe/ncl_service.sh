#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Run the site package's own host (nclink-service) with a driver definition that
# points the plugin at our fake machine, and see what the plugin puts on the
# wire. The host knows the plugin ABI, so this sidesteps the harness mismatch.
#
#   /svc     read-only mount of .../app1/nclink-service
#   /boxcfg  read-only mount of .../INCBOX200/cfg   (models + driver_defs)
#   /work    this directory
set -e

# Everything runs from the container's own filesystem: a 32-bit process
# fstat()ing a file on a Windows bind mount gets EOVERFLOW ("Value too large
# for defined data type"), which is what spdlog trips over.
work=/tmp/run
rm -rf "$work"
mkdir -p "$work/cfg" "$work/log"
cp -r /svc "$work/svc"
cd "$work/svc"

# GSK: model items + a driver definition aimed at the fake machine.
cp /boxcfg/models/gsk.json "$work/cfg/model.json"
cat >"$work/cfg/driver_def.json" <<'JSON'
{
  "connections": [
    {
      "id": "gsk-client-1",
      "module": "gsk-http",
      "parameters": {
        "server": "http://127.0.0.1:33123",
        "ipAddress": "127.0.0.1",
        "port": 6000,
        "timeout": 5
      }
    }
  ],
  "drivers": { "/": { "client": "gsk-client-1" } }
}
JSON
cat >"$work/cfg/nclink_cfg.json" <<JSON
{
  "mqttIp": "127.0.0.1",
  "mqttPort": 1883,
  "mqttUser": "",
  "mqttPassword": "",
  "logLevel": 5,
  "logLimit": 1048576,
  "logPath": "$work/log/nclink.log",
  "model": "$work/cfg/model.json"
}
JSON

python3 /work/mock.py 6000 >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

echo "== starting nclink-service (20 s)"
LD_LIBRARY_PATH=. timeout 20 ./nclink-service \
    -M "$work/cfg/model.json" -D "$work/cfg/driver_def.json" \
    -C "$work/cfg/nclink_cfg.json" -L "$work/log/nclink-service.log" \
    || echo "== nclink-service exited $?"

sleep 1
kill $mock_pid 2>/dev/null || true

echo
echo "===== service log (tail) ====="
tail -25 "$work/log/nclink-service.log" 2>/dev/null || echo "(no log)"
echo
echo "===== what the plugin sent to the fake machine ====="
cat "$work/mock.log"
