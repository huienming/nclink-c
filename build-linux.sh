#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# NC-Link core - Linux build without CMake.
#
#   ./build-linux.sh [输出目录]        # 默认 build-linux
#
# 产出：
#   <输出目录>/libnclink_core.a        静态库
#   <输出目录>/bin/*                   示例与测试可执行文件
# 并运行全部测试套件，最后打印汇总。
#
# 只依赖 gcc/binutils/make 之外的标准工具（find、ar、sh），无需 cmake。

set -e

CC=${CC:-gcc}
AR=${AR:-ar}
OUT=${1:-build-linux}
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

CFLAGS="-std=c11 -O2 -Wall -Wextra -Wshadow -Wstrict-prototypes"
CFLAGS="$CFLAGS -Wmissing-prototypes -Iinclude -Isrc"
# -std=c11 会隐藏 POSIX 接口（strdup/getaddrinfo/localtime_r/pthread 等），
# 必须显式打开；CMake 工程里同样设置了这一项。
CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L"
# 库内部统一用 snprintf 写定长路径缓冲（NCL_PATH_MAX_BUF = 4096），
# GCC 的 -Wformat-truncation 会对这类定长拼接给出大量保守告警，这里显式关闭。
CFLAGS="$CFLAGS -Wno-format-truncation"
LDLIBS="-lpthread"

rm -rf "$OUT/obj"
mkdir -p "$OUT/obj" "$OUT/bin"

echo "== 编译静态库 ($CC) =="
find src -name '*.c' | sort | while read -r src; do
    obj="$OUT/obj/$(echo "$src" | tr '/' '_').o"
    $CC $CFLAGS -c "$src" -o "$obj"
done
$AR rcs "$OUT/libnclink_core.a" "$OUT"/obj/*.o
echo "   -> $OUT/libnclink_core.a"

echo "== 编译示例 =="
for ex in examples/*.c; do
    name=$(basename "$ex" .c)
    $CC $CFLAGS "$ex" -o "$OUT/bin/$name" "$OUT/libnclink_core.a" $LDLIBS
    echo "   -> $OUT/bin/$name"
done

echo "== 编译并运行测试 =="
pass=0
fail=0
for t in tests/test_*.c; do
    name=$(basename "$t" .c)
    extra=""
    case "$name" in
        test_client|test_server) extra="tests/fake_nclink_server.c" ;;
    esac
    case "$name" in
        test_model|test_message)
            extra="$extra -DNCL_TEST_DATA_DIR=\"$ROOT/tests/data\"" ;;
    esac
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS "$t" $extra -o "$OUT/bin/$name" \
            "$OUT/libnclink_core.a" $LDLIBS 2>"$OUT/bin/$name.build.log"; then
        echo "   [编译失败] $name"; tail -5 "$OUT/bin/$name.build.log"; fail=$((fail+1)); continue
    fi
    if (cd "$OUT/bin" && ./"$name" >"$name.log" 2>&1); then
        echo "   [通过] $name"
        pass=$((pass+1))
    else
        echo "   [失败] $name"; tail -8 "$OUT/bin/$name.log"; fail=$((fail+1))
    fi
done

echo
echo "测试：通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
