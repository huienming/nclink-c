#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 按地址段反汇编一个 ARM ELF（给 go_pclntab 认不出的那几份二进制用）。
#
#   sh /work/arm_range.sh /hp2x/hp2x_box200 0x64a918 0x64ac00
#   OUT=/work/_out/x.txt sh /work/arm_range.sh ...
set -e

if [ -n "$OUT" ]; then
    objdump -d --start-address="$2" --stop-address="$3" "$1" >"$OUT" 2>&1
    echo "wrote $OUT"
else
    objdump -d --start-address="$2" --stop-address="$3" "$1"
fi
