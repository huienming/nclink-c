#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""从 FANUC 官方 SDK 的文档包（`Document/SpecE/**/*.xml`）里把所有函数的**原型**抄出来。

每份 XML 都有 `<prottype>` 一行，就是 C 侧的真原型。核 item 的时候最费时间的
其实是"探针该按什么形状调它"（`cnc_rdtofs` 是 4 个参数还是 5 个、第 3 个 short 是
长度还是编号）——原型一列出来就不用猜了；`focas_sdk_probe.c` 里那些"通用形状"
（s1/s2/s3/s1_n/…）就是照这张表配的。

    python fwlib_proto.py <SpecE 目录> [函数名...]
    python fwlib_proto.py <SpecE 目录>                # 全列

文档包在官方 SDK 包的 `Document/SpecE/` 下（`SpecJ/` 是日文版，同一套原型）。
"""
import glob
import io
import os
import re
import sys


def load_protos(root):
    """{函数名: 原型}。同名取第一个（SpecE 下每个函数只有一份）。"""
    pat = re.compile(r"<prottype>\s*(.*?)\s*</prottype>", re.S)
    seen = {}
    for path in sorted(glob.glob(os.path.join(root, "**", "*.xml"),
                                 recursive=True)):
        try:
            text = io.open(path, encoding="cp932", errors="replace").read()
        except OSError:
            continue
        m = pat.search(text)
        if m is None:
            continue
        name = os.path.basename(path)[:-4]
        seen.setdefault(name, re.sub(r"\s+", " ", m.group(1)))
    return seen


def main(argv):
    if len(argv) < 1:
        raise SystemExit(__doc__)
    seen = load_protos(argv[0])
    want = argv[1:] if len(argv) > 1 else sorted(seen)
    for name in want:
        print("%-22s %s" % (name, seen.get(name, "(没找到)")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
