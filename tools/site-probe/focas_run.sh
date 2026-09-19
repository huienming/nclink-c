#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Run inside an armv7 container: build the probe, start the fake machine, drive
# FANUC's own FOCAS client at it and print what the client put on the wire.
set -e

echo "== arch: $(uname -m)"
echo "== libstdc++: $(ls /usr/lib/arm-linux-gnueabihf/libstdc++.so.6 2>/dev/null || echo missing)"

cd /work
gcc -O0 -g -o probe probe.c -ldl
echo "== built probe"

# FOCAS wants its log file (fwlibeth.log) in the working directory; the box
# ships it next to the binaries, we just create it.
: >/work/fwlibeth.log

python3 /work/mock.py 8193 >/work/mock.log 2>&1 &
sleep 1

export LD_LIBRARY_PATH=/svc
./probe 127.0.0.1 8193 || echo "== probe exited $?"
sleep 2

echo
echo "===== what the client sent ====="
cat /work/mock.log
