#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""从 FANUC 官方头（Fwlib64.h / Fwlib32.h）里抠出某个结构体的定义。

这些结构体是"应答载荷怎么切"的**上游口径**：FOCAS 的 Cb 应答基本就是结构体
按字段顺序铺、每个整数大端。所以核某个 item 的布局时，先看结构体长什么样，
再去假机床/探针上把字段偏移对一遍（`focas_sdk_probe.ps1`）。

    python fwlib_struct.py <Fwlib64.h> IODBTOFS ODBTLIFE ODBALMMSG2
    python fwlib_struct.py <Fwlib64.h> --list          # 列所有结构体名
    python fwlib_struct.py <Fwlib64.h> --grep tlife     # 按名字子串找

头文件是 Shift_JIS（FANUC 的发行版），按 cp932 读、看不懂的字节换成占位符。
"""
import io
import re
import sys


def load(path):
    with io.open(path, "r", encoding="cp932", errors="replace") as fp:
        return fp.read()


def structs(text):
    """[(名字, 定义文本)] —— 名字取 `} NAME;` 里那个。"""
    out = []
    for m in re.finditer(r"/\*[^*]*\*/\s*typedef\s+struct\s+\w*\s*\{(.*?)\}\s*"
                         r"(\w+)\s*;", text, re.S):
        out.append((m.group(2), m.group(0)))
    return out


def main(argv):
    if len(argv) < 1:
        raise SystemExit(__doc__)
    text = load(argv[0])
    found = structs(text)
    if len(argv) == 1 or argv[1] == "--list":
        for name, _ in found:
            print(name)
        return 0
    if argv[1] == "--grep":
        pat = argv[2].lower()
        for name, body in found:
            if pat in name.lower():
                print("--- %s\n%s\n" % (name, tidy(body)))
        return 0
    by_name = {name: body for name, body in found}
    for want in argv[1:]:
        body = by_name.get(want)
        if body is None:
            # 结构体名在头里可能写成 `ODBTLIFE2` 之类，容错一下
            hits = [n for n in by_name if n.lower() == want.lower()]
            body = by_name[hits[0]] if hits else None
        if body is None:
            print("--- %s  (头里没有)" % want)
            continue
        print("--- %s\n%s\n" % (want, tidy(body)))
    return 0


def tidy(body):
    lines = []
    for line in body.splitlines():
        line = re.sub(r"/\*.*?\*/", "", line).rstrip()
        if line.strip():
            lines.append(line)
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
