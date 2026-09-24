#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the shared native shim (libnclink_shim.so) used by the C#, Java and
# Python bindings.
#
#   sh build-shim.sh [path/to/libnclink_core.a] [out_dir]
#
# Defaults to <repo>/build-linux/libnclink_core.a (see build-linux.sh) and
# <here>/bin as the output directory.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
core=${1:-$root/build-linux/libnclink_core.a}
out=${2:-$here/bin}

if [ ! -f "$core" ]; then
    echo "missing native core library: $core (run ./build-linux.sh first)" >&2
    exit 1
fi

mkdir -p "$out"
cc -shared -fPIC -O2 -Wall -I"$root/stack/include" \
   -o "$out/libnclink_shim.so" "$here/nclink_shim.c" "$core" -lpthread
echo "native shim: $out/libnclink_shim.so"
