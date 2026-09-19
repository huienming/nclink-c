#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 批量看若干函数里的"栈上字面量"（交给 arm_bytes.py）。
#   sh /work/arm_literals.sh '(*MitsubishiCncM70).GetFeedSpeed' ...
#   /hp2x  read-only mount of .../app1/hp2x
set -e

BIN=${BIN:-/hp2x/hp2x_box200}
SECTION=${SECTION:-字面量}
for name in "$@"; do
    echo "##### $name"
    python3 /work/arm_bytes.py "$BIN" "$name" |
        sed -n "/$SECTION/,\$p"
done
