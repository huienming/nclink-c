#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Long soak: one Monte Carlo process per static pool size, all running at the
# same time, each hammering its own pool with random sizes and random
# allocate/release/resize sequences.
#
#   ./tools/soak-linux.sh                  # one hour, the default pool sizes
#   ./tools/soak-linux.sh --docker         # same, inside gcc:13 (Windows/macOS)
#   NCL_SOAK_SECONDS=600 ./tools/soak-linux.sh
#   NCL_SOAK_MT=1 NCL_SOAK_SECONDS=600 NCL_SOAK_SIZES="65536 1048576" \
#       NCL_MEM_MT_THREADS=8 ./tools/soak-linux.sh   # multithreaded instead
#
# Every process checks after each operation that the pool is still consistent
# (ncl_mem_check()), that the accounting identity holds, that a refusal really
# means "the biggest hole is smaller than the request", and every round drains
# the pool completely and verifies it came back as a single free block. A round
# that is not clean fails the process (non-zero exit) and stops it.
#
# Logs: build-soak/<size>.log . The script prints a summary line per pool size
# and exits non-zero if any process found a problem.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECONDS_TO_RUN=${NCL_SOAK_SECONDS:-3600}
SIZES=${NCL_SOAK_SIZES:-"16384 32768 65536 131072 524288 4194304 20971520"}
OUT="$ROOT/build-soak"
CFLAGS="-std=c11 -O2 -Wall -Wextra -I$ROOT/stack/include -I$ROOT/stack/src"
CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L -Wno-format-truncation"

if [ "${1:-}" = "--docker" ]; then
    exec docker run --rm -v "$ROOT:/work" -w /work gcc:13 sh "$(basename "$0")"
fi

# Two flavours of the same idea: the single threaded Monte Carlo (one process
# per pool size) and the multithreaded one (several threads per process, all
# sharing that pool, handing blocks across threads).
if [ "${NCL_SOAK_MT:-0}" = "1" ]; then
    TEST_SRC="$ROOT/stack/test/core/test_mem_mt.c"
    SECONDS_VAR="NCL_MEM_MT_SECONDS"
    OPS_VAR="NCL_MEM_MT_OPS"
    # The build directory carries the mode: the single threaded and the
    # multithreaded soak are different binaries for the same pool size, and
    # reusing one for the other would silently test the wrong thing.
    OUT_SUB="mt"
else
    TEST_SRC="$ROOT/stack/test/core/test_mem_mc.c"
    SECONDS_VAR="NCL_MEM_MC_SECONDS"
    OPS_VAR="NCL_MEM_MC_OPS"
    OUT_SUB="st"
fi
OUT="$ROOT/build-soak/$OUT_SUB"

cd "$ROOT"
mkdir -p "$OUT"

# Build one library + one test binary per pool size. A pool size is a compile
# time constant, so this is the only way to run several of them at once.
for size in $SIZES; do
    dir="$OUT/$size"
    mkdir -p "$dir"
    if [ ! -x "$dir/soak" ]; then
        echo "building pool $size"
        for f in $(find src -name '*.c'); do
            gcc $CFLAGS -DNCL_STATIC_MEM=1 -DNCL_MEM_POOL_BYTES=$size \
                -DNCL_MEM_REPORT=1 -c "$f" -o "$dir/$(echo "$f" | tr '/' '_').o" || exit 1
        done
        ar rcs "$dir/libncl.a" "$dir"/*.o
        gcc $CFLAGS -DNCL_STATIC_MEM=1 -DNCL_MEM_POOL_BYTES=$size \
            -DNCL_MEM_REPORT=1 "$TEST_SRC" -o "$dir/soak" \
            "$dir/libncl.a" -lpthread -lm || exit 1
    fi
done

# Small pools are cheap to check after every operation; the big ones pay for a
# full walk, so they check every so many operations instead.
stride_for() {
    case "$1" in
        4194304) echo 16 ;;
        20971520) echo 64 ;;
        *) echo 1 ;;
    esac
}

pids=""
seed=1000003
for size in $SIZES; do
    log="$OUT/$size.log"
    : > "$log"
    env "$SECONDS_VAR=$SECONDS_TO_RUN" \
        NCL_MEM_MC_SEED=$seed \
        NCL_MEM_MT_LABEL="[$size]" \
        NCL_MEM_MT_THREADS="${NCL_MEM_MT_THREADS:-8}" \
        NCL_MEM_MT_OPS="${NCL_MEM_MT_OPS:-20000}" \
        NCL_MEM_MC_LABEL="[$size]" \
        "$OPS_VAR=${NCL_MEM_MC_OPS:-20000}" \
        NCL_MEM_MC_CHECK_EVERY=$(stride_for "$size") \
        "$OUT/$size/soak" > "$log" 2>&1 &
    pids="$pids $!"
    seed=$((seed + 7919))
done

echo "soak started: $SECONDS_TO_RUN s, pool sizes: $SIZES"
echo "logs: $OUT/<size>.log"
fail=0
for pid in $pids; do
    wait "$pid" || fail=$((fail + 1))
done

echo "--- summary ---"
for size in $SIZES; do
    line=$(grep -hE '^\[soak|^\[mt' "$OUT/$size.log" | tail -1)
    checks=$(grep -h 'checks,' "$OUT/$size.log" | tail -1 | sed 's/.*: //')
    printf '%9s  %s  [%s]\n' "$size" "$line" "$checks"
done
echo "soak: $fail process(es) failed"
[ "$fail" = "0" ]
