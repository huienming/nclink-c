#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 反汇编 s7v2 里的几个函数（用 .gopclntab 定范围，objdump 出 ARM 汇编）。
# 默认给 Execution / Mode / FeedActual；传参就换：
#   sh /work/s7ncu_disasm.sh '(*S7).ResolveRes' '(*S7).Alarm'
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

BIN=/hp2x/hp2x_box200
WORK=/tmp/dis
rm -rf "$WORK"
mkdir -p "$WORK"

if [ "$#" -eq 0 ]; then
    set -- '(*S7).Execution' '(*S7).Mode' '(*S7).FeedActual'
fi

python3 /work/go_pclntab.py "$BIN" --list 's7v2\.\(\*S7\)' >"$WORK/list.txt" || true

for name in "$@"; do
    line=$(python3 /work/go_pclntab.py "$BIN" --dump "$name" --out "$WORK/fn.bin")
    echo "=== $line"
    start=$(echo "$line" | awk '{print $2}')
    objdump -D -b binary -m arm --adjust-vma="$start" "$WORK/fn.bin" |
        grep -v '^$' | sed -n '7,$p'
done
