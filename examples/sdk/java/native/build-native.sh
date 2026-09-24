#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Build the Java native library (libnclink_jni.so): JNI glue + shared shim.
#
#   sh build-native.sh [path/to/libnclink_core.a] [out_dir]
#
# Defaults to <repo>/build-linux/libnclink_core.a and <here>/bin.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
core=${1:-$root/build-linux/libnclink_core.a}
out=${2:-$here/bin}

if [ ! -f "$core" ]; then
    echo "missing native core library: $core (run ./build-linux.sh first)" >&2
    exit 1
fi

if [ -z "$JAVA_HOME" ]; then
    echo "JAVA_HOME is not set (needed for jni.h)" >&2
    exit 1
fi

mkdir -p "$out"
cc -shared -fPIC -O2 -Wall \
   -I"$root/stack/include" -I"$root/examples/sdk/native" \
   -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
   -o "$out/libnclink_jni.so" \
   "$here/nclink_jni.c" "$root/examples/sdk/native/nclink_shim.c" "$core" -lpthread
echo "java native library: $out/libnclink_jni.so"
