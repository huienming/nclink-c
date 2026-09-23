#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Configure-free Java binding build: native library (JNI + shim) and javac.
#
#   ./build.sh                # native + classes + self-test
#   ./build.sh --skip-native  # javac only (native library already built)
#   ./build.sh --no-test      # do not run the self-test
#   ./build.sh --core-lib <path>
set -e

here=$(cd "$(dirname "$0")" && pwd)
classes="$here/build/classes"
libdir="$here/native/bin"

skip_native=0
run_test=1
core=""
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-native) skip_native=1 ;;
        --no-test) run_test=0 ;;
        --core-lib) shift; core=$1 ;;
        *) echo "usage: $0 [--skip-native] [--no-test] [--core-lib <path>]" >&2; exit 2 ;;
    esac
    shift
done

if [ "$skip_native" = "0" ]; then
    sh "$here/native/build-native.sh" ${core:+"$core"}
fi

command -v javac >/dev/null 2>&1 || { echo "javac not found: install a JDK" >&2; exit 1; }

rm -rf "$classes"
mkdir -p "$classes"
# Java 8 bytecode: 老工程/产线机器上也能用
find "$here/src" "$here/demo" "$here/tests" -name '*.java' > "$here/build/sources.txt"
javac -encoding UTF-8 --release 8 -d "$classes" @"$here/build/sources.txt"
echo "classes: $classes"

if [ "$run_test" = "1" ]; then
    java -Dfile.encoding=UTF-8 -Djava.library.path="$libdir" -cp "$classes" com.nclink.SelfTest
fi