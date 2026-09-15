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
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DST="$ROOT/bindings/go/lib"

mkdir -p "$DST/linux-amd64" "$DST/windows-amd64"

stage() {
    src=$1
    dst=$2
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        echo "  staged $dst"
    fi
}

stage "$ROOT/build-linux/libnclink_core.a"     "$DST/linux-amd64/libnclink_core.a"
stage "$ROOT/build-linux-tls/libnclink_core.a" "$DST/linux-amd64/libnclink_core_tls.a"
stage "$ROOT/build-mingw/libnclink_core.a"     "$DST/windows-amd64/libnclink_core.a"

echo "done: $DST"
