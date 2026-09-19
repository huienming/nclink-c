#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# FOCAS 第三轮：握手已经通了（focas_handshake_probe2.sh 里 rc=0），这一轮
# 一个 SDK 函数一个 SDK 函数地跑，把它发的**设备侧请求**逐条抓下来。
#
#   sh /work/focas_data_probe.sh                 # 默认那一串
#   sh /work/focas_data_probe.sh cnc_statinfo    # 只跑一个
#
#   /svc    read-only mount of .../app1/FANUC/FOCAS2
set -e

cd /work
run=/tmp/run
mkdir -p "$run"
[ -n "$OUT" ] && exec >"$OUT" 2>&1
gcc -O0 -g -o probe focas_probe.c -ldl
: >/work/fwlibeth.log
export LD_LIBRARY_PATH=/svc

resp() { printf 'a0a0a0a00001%s%s%04x%s' "$1" "$2" "$((${#3} / 2))" "$3"; }

# 块长（hex）+ 每块载荷（从块 [16..) 开始铺）。用 mock.py 的 CBREP: 规格：
#   块个数 = 请求里的 Cb 个数，所以块长 0x22 / 0x30 都行，只要 ≥ 34。
BLKSZ=${BLKSZ:-22}
PAY=${PAY:-}

# func 01：体 = 16 字节头（[2..4)=0 → else 分支，[8..10)=记录数 0）
F1=$(resp 01 02 "00000000000000000000000000000000")
REPLY="MAP:6:1:01=$F1|21=CBREP:$BLKSZ:$PAY|02=CBREP:$BLKSZ:$PAY|$F1"

echo "（func 21/02 应答 = CBREP: 块长 0x$BLKSZ，块个数跟着请求的 Cb 个数）"

if [ "$#" -gt 0 ]; then
    FNS="$*"
else
    FNS="cnc_statinfo cnc_machine cnc_rdcount cnc_actf cnc_acts cnc_rdaxisdata
         cnc_rdparam cnc_rdmacro cnc_rdalmmsg2 cnc_rdtofs cnc_rdprogdir3
         cnc_rdexecprog cnc_exeprgname2 cnc_rdblkcount cnc_rdlife"
fi

for fn in $FNS; do
    printf '%s' "$REPLY" >"$run/reply.txt"
    echo "########## $fn"
    python3 -u /work/mock.py 8193 "@$run/reply.txt" >"$run/m.log" 2>&1 &
    mp=$!
    # qemu 里 python 起得慢，等它真的 bind 上再打
    n=0
    while [ $n -lt 100 ]; do
        grep -q 'listening' "$run/m.log" 2>/dev/null && break
        sleep 0.2
        n=$((n + 1))
    done
    sleep 0.3
    ./probe 127.0.0.1 8193 "$fn" || echo "   probe exited $?"
    sleep 0.3
    kill "$mp" 2>/dev/null || true
    grep -vE '^$' "$run/m.log" | sed 's/^/   /'
    echo
done
