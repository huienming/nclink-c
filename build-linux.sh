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
# NCL_MEM_SINGLE_THREAD=1 可去掉池的锁（单上下文/裸机）；
# NCL_MEM_REPORT=1 让每个测试进程在退出时打印池的峰值占用与最大请求，
# 这是给设备定池大小的实测依据（与 build.ps1 -MemReport 同一份统计）。
# 例如 NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 ./build-linux.sh build-linux-static
#
# 产出：
#   <输出目录>/libnclink_core.a        核心库（含 tool 层：驱动骨架/审计/宿主/模块装载）
#   <输出目录>/libnclink_clients.a     协议客户端库（Modbus/MC/FINS/S7/FOCAS/...）
#   <输出目录>/bin/ncl_server          唯一的设备程序（装载 plugins/ 下的适配器模块）
#   <输出目录>/plugins/*               适配器模块（ncl_driver_<工具名>.dll|.so）
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

# 测试是在 $OUT/bin 里跑的，所以给测试的目录必须是绝对路径：$OUT 允许写成相对
# 路径（默认）或绝对路径（比如容器里挂到 /out），这里统一算一次。
case "$OUT" in
    /*) TEST_OUT_DIR="$OUT" ;;
    *)  TEST_OUT_DIR="$ROOT/$OUT" ;;
esac
TEST_BIN_DIR="$TEST_OUT_DIR/bin"

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
# json.c 的标量取整用 floor/ceil：程序链接时要 -lm（库静态链接时不会自己带进来）
LDLIBS="$LDLIBS -lm"
# mingw 目标（给 Go 的 cgo 用，见 tools/stage-go-libs.sh）要在链接时显式给出
# Windows 的系统库：MSVC 下源码里的 #pragma comment(lib, ...) 会做这件事，
# mingw 不会，所以示例与测试会在链接期报 __imp_WSAGetLastError 之类的未定义。
case "$("$CC" -dumpmachine 2>/dev/null)" in
    *mingw*|*w64*) LDLIBS="$LDLIBS -lws2_32 -liphlpapi -lwinmm" ;;
esac

# 动态模块的命名要与 ncl_library_file_name() 的约定一致：Windows 无前缀 + .dll，
# 其它平台 lib 前缀 + .so。插件与测试夹具都用这两个变量。
case "$("$CC" -dumpmachine 2>/dev/null)" in
    *mingw*|*w64*|*windows*)
        MODPREFIX=""; MODSUF=".dll" ;;
    *)
        MODPREFIX="lib"; MODSUF=".so" ;;
esac

# TLS 是可选的：默认零依赖，打开后链接系统 OpenSSL，用于 MQTT over ssl://。
if [ "${NCL_WITH_TLS:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DNCL_WITH_TLS=1"
    LDLIBS="$LDLIBS -lssl -lcrypto"
fi
# 交叉构建（mingw 之类）时 OpenSSL 不在系统里：给 NCL_OPENSSL_ROOT 指一份自编的
# （默认前缀，即 <root>/include/openssl 与 <root>/lib/libssl.a），与 build.ps1 的
# -OpenSslRoot 同一个意思。静态 OpenSSL 还要自己带系统库：NCL_EXTRA_LIBS。
if [ -n "${NCL_OPENSSL_ROOT:-}" ]; then
    CFLAGS="$CFLAGS -I$NCL_OPENSSL_ROOT/include"
    LDLIBS="$LDLIBS -L$NCL_OPENSSL_ROOT/lib"
fi
if [ -n "${NCL_EXTRA_LIBS:-}" ]; then
    LDLIBS="$LDLIBS $NCL_EXTRA_LIBS"
fi

# 静态内存：库内所有分配走 ncl_mem_*，打开后由固定池供给，
# 池的地址空间也在静态区里，整个库不再向堆要一个字节。
if [ "${NCL_STATIC_MEM:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DNCL_STATIC_MEM=1 -DNCL_MEM_POOL_BYTES=${NCL_MEM_POOL_BYTES:-20971520}"
    if [ "${NCL_MEM_SINGLE_THREAD:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DNCL_MEM_SINGLE_THREAD=1"
    fi
    if [ "${NCL_MEM_REPORT:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DNCL_MEM_REPORT=1"
    fi
fi

rm -rf "$OUT/obj"
mkdir -p "$OUT/obj" "$OUT/bin"

echo "== 编译静态库 ($CC) =="
find src -name '*.c' ! -path 'src/tool/main.c' | sort | while read -r src; do
    obj="$OUT/obj/$(echo "$src" | tr '/' '_').o"
    $CC $CFLAGS -c "$src" -o "$obj"
done
# 先删掉旧归档：ar rcs 只增删它列出的成员，源文件搬过地方（改名/换目录）就会留下
# 旧成员，于是同一个符号出现两份 —— 链接期才报 multiple definition。
rm -f "$OUT/libnclink_core.a"
$AR rcs "$OUT/libnclink_core.a" "$OUT"/obj/*.o
echo "   -> $OUT/libnclink_core.a"


# 协议客户端库（clients/）：厂商协议的报文与会话，一个协议一个目录，只认字节和
# 会话，不认识 NC-Link。适配器模块引用它们去跟机床说话。
CLI_CFLAGS="-Iclients -Iclients/include"
CLI_OBJDIR="$OUT/obj-clients"
rm -rf "$CLI_OBJDIR"
mkdir -p "$CLI_OBJDIR"
echo "== 编译协议客户端库 =="
for src in $(find clients -maxdepth 2 -name '*.c' ! -path 'clients/tests/*' | sort); do
    obj="$CLI_OBJDIR/$(echo "$src" | tr '/' '_').o"
    $CC $CFLAGS $CLI_CFLAGS -c "$src" -o "$obj"
done
rm -f "$OUT/libnclink_clients.a"
$AR rcs "$OUT/libnclink_clients.a" "$CLI_OBJDIR"/*.o
echo "   -> $OUT/libnclink_clients.a"

# 设备程序：唯一的可执行文件。它装载 <root>/plugins 下的适配器模块，把模块声明好
# 的工具注册到设备上，然后跑 MQTT + REST + 轮询。
echo "== 编译设备程序 =="
$CC $CFLAGS src/tool/main.c -o "$OUT/bin/ncl_server" \
    "$OUT/libnclink_core.a" $LDLIBS
echo "   -> $OUT/bin/ncl_server"

# 适配器模块：plugins/<协议>.c 一个文件一个适配器（只 #include nclink/ncl_tool.h），
# 编成可动态装载的模块，ncl_server 启动时按配置里的工具名装载。
mkdir -p "$OUT/plugins"
echo "== 编译适配器模块 =="
for src in plugins/*.c; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .c)
    $CC $CFLAGS $CLI_CFLAGS -shared \
        -o "$OUT/plugins/${MODPREFIX}ncl_driver_$name$MODSUF" \
        "$src" "$OUT/libnclink_clients.a" "$OUT/libnclink_core.a" $LDLIBS
    echo "   -> $OUT/plugins/${MODPREFIX}ncl_driver_$name$MODSUF"
done

# 测试夹具模块：一个 ABI 代次不符、一个根本没有入口（装载器都得拒掉），再加一个
# 声明式适配器夹具（宿主端到端用它）。它们跟真插件一样叫 ncl_driver_<名字>。
# 声明式的那个要连核心库：它用 ncl_json_*/ncl_tool_* 这些核心符号，Linux 上动态
# 装载时这些符号得在模块自己身上找到（CMake 的 MODULE 目标同样把 core 链进去）。
mkdir -p "$OUT/plugins-tool-fixture" "$OUT/plugins-refused-fixture"
$CC $CFLAGS -Iinclude -shared \
    -o "$OUT/plugins-tool-fixture/${MODPREFIX}ncl_driver_test_tool_basic$MODSUF" \
    tests/module_tool_basic.c "$OUT/libnclink_core.a" $LDLIBS
for fixture in bad_abi no_entry; do
    $CC $CFLAGS -Iinclude -shared \
        -o "$OUT/plugins-refused-fixture/${MODPREFIX}ncl_driver_test_$fixture$MODSUF" \
        "tests/module_$fixture.c"
done
echo "== 编译示例 =="
for ex in examples/*.c; do
    # device_model.c 不是程序：它是设备模型（被示例与垫片 #include 进去的）
    if [ "$ex" = "examples/device_model.c" ]; then
        continue
    fi
    name=$(basename "$ex" .c)
    # 设备端示例要把模型一起编进去（模型是它的一部分）
    extra=""
    if [ "$name" = "ncl_device_demo" ] || [ "$name" = "ncl_file_bench" ]; then
        extra="examples/device_model.c"
    fi
    $CC $CFLAGS "$ex" $extra -o "$OUT/bin/$name" "$OUT/libnclink_core.a" $LDLIBS
    echo "   -> $OUT/bin/$name"
done

echo "== 编译并运行测试 =="
pass=0
fail=0
built=0
# NCL_RUN_TESTS=0：照旧逐个编译测试，但不执行（交叉构建出来的 PE/别的目标本机跑
# 不起来；产物拷到目标平台上再跑一遍）。编译失败照样算 fail。
RUN_TESTS=${NCL_RUN_TESTS:-1}
CXX=${CXX:-g++}

# 动态加载的夹具模块（tests/test_library.c 要装载它；文件名与测试里的拼法一致）
$CC $CFLAGS -shared -o "$OUT/bin/ncl_test_module$MODSUF" tests/test_module.c

for t in tests/test_*.c; do
    name=$(basename "$t" .c)
    # 夹具模块不是测试：它没有 main，只给 test_library 当被装载的对象
    if [ "$name" = "test_module" ]; then
        continue
    fi
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
    case "$name" in
        # 绝对路径：测试是在 $OUT/bin 里跑的，相对路径会变成 bin/bin
        test_library)
            extra="$extra -DNCL_TEST_MODULE_DIR=\"$TEST_BIN_DIR\"" ;;
    esac
    case "$name" in
        # 宿主端到端：装载声明的适配器夹具（含两个必须被拒的），绝对路径同上
        test_host_tool)
            extra="$extra -DNCL_TEST_PLUGIN_DIR=\"$TEST_OUT_DIR/plugins-tool-fixture\" -DNCL_TEST_MODULE_DIR=\"$TEST_OUT_DIR/plugins-refused-fixture\"" ;;
    esac
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS -Itests -Iclients "$t" $extra -o "$OUT/bin/$name" \
            "$OUT/libnclink_clients.a" "$OUT/libnclink_core.a" $LDLIBS 2>"$OUT/bin/$name.build.log"; then
        echo "   [编译失败] $name"; tail -5 "$OUT/bin/$name.build.log"; fail=$((fail+1)); continue
    fi
    if [ "$RUN_TESTS" = "0" ]; then
        echo "   [仅编译] $name"; built=$((built+1)); continue
    fi
    if (cd "$OUT/bin" && ./"$name" >"$name.log" 2>&1); then
        echo "   [通过] $name"
        pass=$((pass+1))
    else
        echo "   [失败] $name"; tail -8 "$OUT/bin/$name.log"; fail=$((fail+1))
    fi
done

# 协议客户端库测试（clients/tests）：黄金帧与靶机，链接同一个静态库。
for t in clients/tests/test_*.c; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .c)
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS $CLI_CFLAGS -Itests "$t" -o "$OUT/bin/$name" \
            "$OUT/libnclink_clients.a" "$OUT/libnclink_core.a" $LDLIBS \
            2>"$OUT/bin/$name.build.log"; then
        echo "   [编译失败] $name"; tail -5 "$OUT/bin/$name.build.log"; fail=$((fail+1)); continue
    fi
    if [ "$RUN_TESTS" = "0" ]; then
        echo "   [仅编译] $name"; built=$((built+1)); continue
    fi
    if (cd "$OUT/bin" && ./"$name" >"$name.log" 2>&1); then
        echo "   [通过] $name"
        pass=$((pass+1))
    else
        echo "   [失败] $name"; tail -8 "$OUT/bin/$name.log"; fail=$((fail+1))
    fi
done

if [ "${NCL_BUILD_CPP:-1}" = "1" ] && command -v "$CXX" >/dev/null 2>&1; then
    for t in tests/test_*.cpp; do
        [ -e "$t" ] || continue
        name=$(basename "$t" .cpp)
        # shellcheck disable=SC2086
        if ! $CXX -std=c++"${NCLINK_CXX_STANDARD:-17}" -Wall -Wextra -Iinclude -Itests "$t" \
                "$OUT/libnclink_core.a" $LDLIBS -o "$OUT/bin/$name" \
                2>"$OUT/bin/$name.build.log"; then
            echo "   [编译失败] $name"; tail -5 "$OUT/bin/$name.build.log"
            fail=$((fail+1)); continue
        fi
        if [ "$RUN_TESTS" = "0" ]; then
            echo "   [仅编译] $name"; built=$((built+1)); continue
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
if [ "$RUN_TESTS" = "0" ]; then
    echo "测试：已编译 $built，失败 $fail（NCL_RUN_TESTS=0，本机不执行；产物在目标平台跑）"
else
    echo "测试：通过 $pass，失败 $fail"
fi
[ "$fail" -eq 0 ]
