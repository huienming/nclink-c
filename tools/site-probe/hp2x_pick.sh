#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 现场包在本机有好几份，先认出"文档里那些地址"是哪一份 hp2x_box200。
#
#   sh /work/hp2x_pick.sh <候选路径...>
set -e

[ -n "$OUT" ] && exec >"$OUT" 2>&1

for b in "$@"; do
    [ -f "$b" ] || { echo "=== $b  (没有)"; continue; }
    echo "=== $b  ($(stat -c%s "$b") bytes)"
    echo "    s7v2 串命中: $(strings -a "$b" | grep -c 's7v2' || true)"
    echo "    0x648060 处:"
    objdump -d --start-address=0x648060 --stop-address=0x6480a0 "$b" |
        sed -n '7,14p' | sed 's/^/      /'
done
