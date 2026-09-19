#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Build the plugin driver and point GSK's HTTP plugin at a fake machine on 6000.
cd /work || exit 1
lib=${1:-libgsk-http.so}
port=${2:-6000}

g++ -w -O0 -o drive drive.cpp -ldl || exit 1
: >fwlibeth.log

python3 mock.py "$port" >mock.log 2>&1 &
pid=$!
sleep 0.8

echo "=== driving $lib at 127.0.0.1:$port"
LD_LIBRARY_PATH=/svc ./drive "/svc/$lib" 127.0.0.1 "$port" "${3:-/GSK/CNC/Open/TCP}" 2>&1 |
    sed -n '1,12p'

sleep 0.5
kill $pid 2>/dev/null
wait $pid 2>/dev/null

echo "=== what the plugin sent to the machine"
cat mock.log
