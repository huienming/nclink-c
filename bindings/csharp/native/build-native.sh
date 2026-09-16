#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the C# native shim as a shared library (Linux / macOS).
#
#   sh build-native.sh [path/to/libnclink_core.a]
#
# Defaults to <repo>/build-linux/libnclink_core.a (see build-linux.sh).
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
core=${1:-$root/build-linux/libnclink_core.a}
out="$here/bin"

if [ ! -f "$core" ]; then
    echo "missing native core library: $core (run ./build-linux.sh first)" >&2
    exit 1
fi

mkdir -p "$out"
cc -shared -fPIC -O2 -Wall -I"$root/include" \
   -o "$out/libnclink_shim.so" "$here/nclink_shim.c" "$core" -lpthread
echo "native shim: $out/libnclink_shim.so"
