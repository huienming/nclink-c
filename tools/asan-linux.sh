#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# AddressSanitizer + LeakSanitizer run over the library and the suites that
# stress allocation the hardest, including the multithreaded one. Add --tsan
# to run the allocator's concurrency test under ThreadSanitizer instead (the
# container needs --privileged for that: TSan and ASLR do not get along).
#
#   ./tools/asan-linux.sh              # native (needs gcc with libasan)
#   ./tools/asan-linux.sh --docker     # inside gcc:13, from Windows or macOS
#   ./tools/asan-linux.sh --tsan --docker   # ThreadSanitizer, allocator only
#
# Why this exists next to the Windows sanitizer run: MSVC's ASan has no leak
# detection, so a Linux run is the only one that reports leaked objects. It is
# what found the leaked reader thread on MQTT reconnect and the REST attach
# context (see CHANGELOG).
#
# Output: builds/build-asan/<name>.log per suite, printed summary at the end.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=${NCL_ASAN_DIR:-$ROOT/builds/build-asan-linux}
CF="-std=c11 -O0 -g -fsanitize=address -I$ROOT/stack/include -Istack/src -D_POSIX_C_SOURCE=200809L"
CF="$CF -Wno-format-truncation -Wno-implicit-function-declaration"

MODE=asan
for arg in "$@"; do
    case "$arg" in
        --tsan) MODE=tsan ;;
        --docker)
            exec docker run --rm --privileged -v "$ROOT:/work" -w /work gcc:13 \
                sh "$(basename "$0")" $( [ "$MODE" = tsan ] && echo --tsan ) ;;
    esac
done

if [ "$MODE" = "tsan" ]; then
    SAN="-fsanitize=thread"
    D=${NCL_TSAN_DIR:-$ROOT/builds/build-tsan}
    SUITES="test_mem_mt"
else
    SAN="-fsanitize=address"
    D=${NCL_ASAN_DIR:-$ROOT/builds/build-asan-linux}
    SUITES="test_mem test_mem_mt test_message test_mqtt_client test_rest test_client"
fi
CF="-std=c11 -O1 -g $SAN -I$ROOT/stack/include -I$ROOT/stack/src -D_POSIX_C_SOURCE=200809L"
CF="$CF -Wno-format-truncation -Wno-implicit-function-declaration"
[ "$MODE" = "tsan" ] && CF="$CF -DNCL_STATIC_MEM=1 -DNCL_MEM_POOL_BYTES=65536"

cd "$ROOT"
mkdir -p "$D"
for f in $(find src -name '*.c'); do
    gcc $CF -c "$f" -o "$D/$(echo "$f" | tr '/' '_').o"
done
ar rcs "$D/libncl.a" "$D"/*.o

if [ "$MODE" = "tsan" ] && [ ! -w /proc/sys/kernel/randomize_va_space ]; then
    echo "note: run this container with --privileged, TSan needs ASLR off"
fi
if [ "$MODE" = "tsan" ]; then
    # TSan cannot cope with the high entropy Linux 6.6+ uses for mmap; this is
    # the standard container workaround and needs a privileged container.
    sysctl -w vm.mmap_rnd_bits=28 >/dev/null 2>&1 || \
        echo "note: could not lower vm.mmap_rnd_bits, TSan may refuse to start"
fi

fail=0
for t in $SUITES; do
    extra=""
    case "$t" in
        test_client) extra="$ROOT/stack/test/fake_nclink_server.c" ;;
        test_message) extra="-DNCL_TEST_DATA_DIR=\"$ROOT/stack/test/data\"" ;;
        test_mem_mt) extra="-DNCL_MEM_MT_THREADS=${NCL_MEM_MT_THREADS:-6}" ;;
    esac
    # shellcheck disable=SC2086
    gcc $CF -I"$ROOT/stack/test" $extra $(find "$ROOT/stack/test" -name "$t.c" | head -1) -o "$D/$t" "$D/libncl.a" \
        -lpthread -lm
    (cd "$D" && "./$t" >"$t.log" 2>&1) || fail=$((fail + 1))
    leaks=$(grep -c 'ERROR: .*Sanitizer' "$D/$t.log" 2>/dev/null || true)
    echo "  $t: $(grep -E 'checks,' "$D/$t.log" | tail -1)"
    if [ "${leaks:-0}" != "0" ]; then
        echo "    sanitizer findings: $leaks"
        grep -E 'SUMMARY' "$D/$t.log" | head -3
    fi
done
echo "$MODE: $fail suite(s) with sanitizer findings, logs in $D"
[ "$fail" = "0" ]
