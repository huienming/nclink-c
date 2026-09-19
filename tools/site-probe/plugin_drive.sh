#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Build the plugin driver and point GSK's HTTP plugin at a fake machine on 6000.
cd /work || exit 1
lib=${1:-libgsk-http.so}
port=${2:-6000}

# The vendor plugins were built with the **old** libstdc++ string ABI: their
# imports are _ZNSs... (`std::string` = `Ss`), not _ZNSt7__cxx1112basic_string...
# Passing a std::string (or a json containing one) across that boundary corrupts
# the heap, so the harness has to be built with the same ABI.
g++ -w -O0 -D_GLIBCXX_USE_CXX11_ABI=0 -o drive plugin_drive.cpp -ldl || exit 1
: >fwlibeth.log

# 33123 is the plugin's "module server" (the box runs the Go gateway there); the
# plugin refuses to come up if it cannot reach it, so answer 200 with a JSON body.
python3 mock.py 33123 'HTTP200:{"code":0,"msg":"ok","data":{"connectionId":"probe"}}' \
    >module.log 2>&1 &
mod_pid=$!
python3 mock.py "$port" >mock.log 2>&1 &
pid=$!
sleep 0.8

echo "=== driving $lib at 127.0.0.1:$port"
timeout 20 env LD_LIBRARY_PATH=/svc ./drive "/svc/$lib" 127.0.0.1 "$port" \
    "${3:-/GSK/CNC/Open/TCP}" "${4:-}" "${5:-}" 2>&1 || echo "== drive exited $?"

sleep 0.5
kill $pid 2>/dev/null
wait $pid 2>/dev/null
kill $mod_pid 2>/dev/null
wait $mod_pid 2>/dev/null

echo "=== what the plugin sent to the machine"
cat mock.log
echo "=== what the plugin sent to the module server (:33123)"
head -40 module.log
