#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Regenerate bindings/native/nclink_shim.h from bindings/native/nclink_shim.c.

The header is the contract the JNI glue (bindings/java/native/nclink_jni.c)
compiles against, so it has to list exactly the exported functions of the shim.
Generating it from the implementation keeps the two from drifting:

    python tools/gen_shim_header.py           # rewrite the header
    python tools/gen_shim_header.py --check    # fail when it is out of date
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = os.path.join(ROOT, "bindings", "native")
SOURCE = os.path.join(NATIVE, "nclink_shim.c")
HEADER = os.path.join(NATIVE, "nclink_shim.h")

PREAMBLE = """\
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * nclink_shim 的声明（生成物：python tools/gen_shim_header.py）。
 *
 * 这个头文件是给**编译期**用的：Java 的 JNI 胶水（nclink_jni.c）把它和
 * nclink_shim.c 一起编进 nclink_jni.dll。C# / Python 走的是运行时查找（P/Invoke /
 * ctypes），只需要保证垫片 DLL 在搜索路径上。
 *
 * 约定见 nclink_shim.c 顶部：返回 char* 的要 nclshim_free()，返回 const char* /
 * const void* 的是借用指针，回调里的 message 句柄只在回调期间有效。
 */
#ifndef NCLINK_SHIM_H
#define NCLINK_SHIM_H

#if defined(_WIN32)
#define NCLSHIM_API __declspec(dllexport)
#else
#define NCLSHIM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

"""

EPILOGUE = """\

#ifdef __cplusplus
}
#endif

#endif /* NCLINK_SHIM_H */
"""

SIGNATURE_END = re.compile(r"\)\s*$")


def declarations():
    """Yield (comment_lines, signature_lines) for every exported function."""
    raw = io.open(SOURCE, encoding="utf-8").read().splitlines()
    found = []
    i = 0
    while i < len(raw):
        line = raw[i]
        if not line.startswith("NCLSHIM_API"):
            i += 1
            continue
        # Walk back over the doc comment directly above the declaration.
        comment = []
        j = i - 1
        while j >= 0 and raw[j].strip() != "" and not raw[j].startswith("NCLSHIM_API"):
            comment.insert(0, raw[j])
            if raw[j].lstrip().startswith("/**"):
                break
            j -= 1
        signature = []
        while i < len(raw):
            signature.append(raw[i])
            if SIGNATURE_END.search(raw[i]):
                i += 1
                break
            i += 1
        found.append((comment, signature))
    return found


def build():
    out = [PREAMBLE]
    for comment, signature in declarations():
        if comment:
            out.append("\n".join(comment) + "\n")
        out.append("\n".join(signature) + ";\n\n")
    out.append(EPILOGUE)
    return "".join(out)


def main():
    text = build()
    check = len(sys.argv) > 1 and sys.argv[1] == "--check"
    if check:
        current = io.open(HEADER, encoding="utf-8").read()
        if current != text:
            print("nclink_shim.h is out of date: run python tools/gen_shim_header.py",
                  file=sys.stderr)
            return 1
        print("nclink_shim.h is up to date (%d declarations)" % len(declarations()))
        return 0
    io.open(HEADER, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote %s (%d declarations)" % (HEADER, len(declarations())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
