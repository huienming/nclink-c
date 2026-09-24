#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# 跑解码器 fuzz：
#   ./tools/fuzz.sh              # 有 clang 用 libFuzzer（每个目标 30 秒），
#                                # 否则用随机输入回归（每个目标 5 万次）
#   ./tools/fuzz.sh 60           # libFuzzer 每个目标跑 60 秒
#
# 目标：0 = JSON，1 = NC-Link 消息，2 = MQTT 报文/属性/变长整数

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECONDS_PER_TARGET=${1:-30}
OUT=${2:-"$ROOT/build-fuzz"}
mkdir -p "$OUT"

CFLAGS="-std=c11 -g -O1 -I$ROOT/stack/include -I$ROOT/stack/src -D_POSIX_C_SOURCE=200809L"
LIBS="$OUT/libnclink_core.a"

if [ ! -f "$LIBS" ]; then
    echo "== 编译库 =="
    mkdir -p "$OUT/obj"
    for f in $(find "$ROOT/stack/src" -name '*.c' | sort); do
        cc $CFLAGS -c "$f" -o "$OUT/obj/$(echo "$f" | tr '/' '_').o" || exit 1
    done
    ar rcs "$LIBS" "$OUT"/obj/*.o
fi

if command -v clang >/dev/null 2>&1; then
    echo "== libFuzzer（每目标 ${SECONDS_PER_TARGET}s）=="
    fail=0
    for t in 0 1 2; do
        echo "-- target $t --"
        clang $CFLAGS -fsanitize=fuzzer,address -DNCL_FUZZER -DFUZZ_TARGET=$t \
            "$ROOT/stack/test/fuzz/fuzz_codec.c" "$LIBS" -lpthread -o "$OUT/fuzz$t" || exit 1
        "$OUT/fuzz$t" -max_total_time="$SECONDS_PER_TARGET" -print_final_stats=1 \
            -artifact_prefix="$OUT/" || fail=1
    done
    exit $fail
fi

echo "== 无 clang，改用随机输入回归（每目标 5 万次）=="
for t in 0 1 2; do
    cc $CFLAGS -DFUZZ_TARGET=$t "$ROOT/stack/test/fuzz/fuzz_codec.c" "$LIBS" \
        -lpthread -o "$OUT/regress$t" || exit 1
    "$OUT/regress$t" 50000 || exit 1
done
