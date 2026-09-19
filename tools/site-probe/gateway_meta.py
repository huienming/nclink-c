#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""把网关 Go 二进制里内嵌的路由元数据抄出来（离线可用）。

`hp2x_box200` 用 GoFrame 的 `g.Meta` 声明路由，编译后有些路由会在 .rodata 里
留一段纯文本：

    Meta=path:"/FeedActual" tags:"S7NCU" method:"post" dc:"FeedActual"

**注意**：这类字面量只对**一部分**路由存活（在 hp2x_box200 上只有 5 条），
完整的 201 条还是得跑起来抓 `/api.json`。所以这个脚本的用处是"离线补刀"：
不起网关、不抓包，就能确认某个模块的路径与描述（例如 S7NCU 的 `/FeedActual`），
顺带把所有模块名数一遍。

    python tools/site-probe/gateway_meta.py <hp2x_box200> [--tag S7NCU]

不带 `--tag` 时打印模块计数；带 tag 时打印该模块的这几条。
"""

import re
import sys

PATTERN = re.compile(
    r'Meta=path:"([^"]*)" tags:"([^"]*)" method:"([^"]*)" dc:"([^"]*)"')


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    tag = None
    if "--tag" in argv:
        tag = argv[argv.index("--tag") + 1]

    with open(path, "rb") as handle:
        blob = handle.read().decode("latin-1")
    rows = PATTERN.findall(blob)

    if tag is None:
        counts = {}
        for _, tags, _, _ in rows:
            counts[tags] = counts.get(tags, 0) + 1
        print("%d 条路由，%d 个模块" % (len(rows), len(counts)))
        for name, count in sorted(counts.items(), key=lambda kv: -kv[1]):
            print("  %-22s %d" % (name, count))
        return 0

    picked = [r for r in rows if r[1] == tag]
    print("%s: %d 条" % (tag, len(picked)))
    for route, _, method, desc in sorted(picked):
        print("  %-24s %-5s %s" % (route, method, desc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
