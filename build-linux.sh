#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# NC-Link core - Linux build without CMake.
#
#   ./build-linux.sh [输出目录]        # 默认 build-linux
#
# 可选：NCL_WITH_TLS=1 打开 ssl:// 支持（需要 OpenSSL 的头文件与库），
# 例如 NCL_WITH_TLS=1 ./build-linux.sh build-linux-tls
#
# 可选：NCL_STATIC_MEM=1 让库内每次分配都从固定静态池里拿（不调用 malloc），
# 池大小用 NCL_MEM_POOL_BYTES 指定，默认 20 MiB（20971520 字节）；
# NCL_MEM_SINGLE_THREAD=1 可去掉池的锁（单上下文/裸机）。
# 例如 NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 ./build-linux.sh build-linux-static
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
# -fPIC：静态库也要能链进共享库 —— C# / Java / Python 绑定的原生垫片就是 .so，
# 非 PIC 的 .a 在 x86_64 上会以 "relocation R_X86_64_32 ... recompile with -fPIC"
# 直接链接失败。可执行文件用 PIC 库没有任何问题。
CFLAGS="$CFLAGS -fPIC"
# -std=c11 会隐藏 POSIX 接口（strdup/getaddrinfo/localtime_r/pthread 等），
# 必须显式打开；CMake 工程里同样设置了这一项。
CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L"
# 库内部统一用 snprintf 写定长路径缓冲（NCL_PATH_MAX_BUF = 4096），
# GCC 的 -Wformat-truncation 会对这类定长拼接给出大量保守告警，这里显式关闭。
CFLAGS="$CFLAGS -Wno-format-truncation"
LDLIBS="-lpthread"

# TLS 是可选的：默认零依赖，打开后链接系统 OpenSSL，用于 MQTT over ssl://。
if [ "${NCL_WITH_TLS:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DNCL_WITH_TLS=1"
    LDLIBS="$LDLIBS -lssl -lcrypto"
fi

# 静态内存：库内所有分配走 ncl_mem_*，打开后由固定池供给，
# 池的地址空间也在静态区里，整个库不再向堆要一个字节。
if [ "${NCL_STATIC_MEM:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DNCL_STATIC_MEM=1 -DNCL_MEM_POOL_BYTES=${NCL_MEM_POOL_BYTES:-20971520}"
    if [ "${NCL_MEM_SINGLE_THREAD:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DNCL_MEM_SINGLE_THREAD=1"
    fi
fi

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
    # device_model.c 不是程序：它是设备模型（被示例与垫片 #include 进去的）
    if [ "$ex" = "examples/device_model.c" ]; then
        continue
    fi
    name=$(basename "$ex" .c)
    # 设备端示例要把模型一起编进去（模型是它的一部分）
    extra=""
    if [ "$name" = "ncl_device_demo" ]; then
        extra="examples/device_model.c"
    fi
    $CC $CFLAGS "$ex" $extra -o "$OUT/bin/$name" "$OUT/libnclink_core.a" $LDLIBS
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
    case "$name" in
        test_tls)
            extra="$extra -DNCL_TEST_DATA_DIR=\"$ROOT/tests/data\"" ;;
    esac
    case "$name" in
        test_license)
            extra="$extra -DNCL_TEST_SOURCE_ROOT=\"$ROOT\"" ;;
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

# C++ 封装测试（header-only，需要 g++）
if [ "${NCL_BUILD_CPP:-1}" = "1" ] && command -v g++ >/dev/null 2>&1; then
    for t in tests/test_*.cpp; do
        [ -e "$t" ] || continue
        name=$(basename "$t" .cpp)
        # shellcheck disable=SC2086
        if ! g++ -std=c++"${NCLINK_CXX_STANDARD:-17}" -Wall -Wextra -Iinclude -Itests "$t" \
                "$OUT/libnclink_core.a" $LDLIBS -o "$OUT/bin/$name" \
                2>"$OUT/bin/$name.build.log"; then
            echo "   [编译失败] $name"; tail -5 "$OUT/bin/$name.build.log"
            fail=$((fail+1)); continue
        fi
        if (cd "$OUT/bin" && ./"$name" >"$name.log" 2>&1); then
            echo "   [通过] $name"; pass=$((pass+1))
        else
            echo "   [失败] $name"; tail -8 "$OUT/bin/$name.log"
            fail=$((fail+1))
        fi
    done
fi

echo
echo "测试：通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
