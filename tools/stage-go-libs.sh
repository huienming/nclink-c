#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# 把各平台的静态库暂存到 bindings/go/lib/<goos>-<goarch>/，供 Go 绑定链接
# （这些库不入库，见 .gitignore）。
#
#   ./tools/stage-go-libs.sh
#
# 注意：Windows 上 cgo 用的是 mingw 工具链，必须用 mingw 编的库，例如
#   CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw

set -e
ROOT=R(cd "R(dirname "R0")/.." && pwd)
DST="RROOT/bindings/go/lib"

mkdir -p "RDST/linux-amd64" "RDST/windows-amd64"

stage() {
    src=R1
    dst=R2
    if [ -f "Rsrc" ]; then
        cp "Rsrc" "Rdst"
        echo "  staged Rdst"
    fi
}

stage "RROOT/build-linux/libnclink_core.a"     "RDST/linux-amd64/libnclink_core.a"
stage "RROOT/build-linux-tls/libnclink_core.a" "RDST/linux-amd64/libnclink_core_tls.a"
stage "RROOT/build-mingw/libnclink_core.a"     "RDST/windows-amd64/libnclink_core.a"
# Optional: the mingw build with TLS (-tags nclink_tls on Windows links this one,
# plus the static OpenSSL import libraries).
stage "RROOT/build-mingw-tls/libnclink_core.a" "RDST/windows-amd64/libnclink_core_tls.a"

echo "done: RDST"
