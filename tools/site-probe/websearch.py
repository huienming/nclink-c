#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""查东西用的小脚本：把查询词丢给本机 SearXNG，打印标题/链接/摘要。

    python tools/site-probe/websearch.py "查询词" [更多查询词 ...]

环境变量：
    SEARX    搜索引擎地址（默认 http://localhost:8080）
    N        每个查询词取几条（默认 8）

多个查询词可以一次传进来，脚本顺序执行、分节打印——这一轮查西门子
数据项含义时就是这么用的（SEARXNG 跑在 docker 里的 searxng-core）。
"""

import json
import os
import sys
import urllib.parse
import urllib.request

BASE = os.environ.get("SEARX", "http://localhost:8080")
N = int(os.environ.get("N", "8"))

# Windows 控制台默认 GBK，日文/德文标题会直接抛 UnicodeEncodeError，先掐掉。
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:                       # noqa: BLE001 - 老解释器没有这个
        pass


def search(query):
    url = BASE + "/search?" + urllib.parse.urlencode(
        {"q": query, "format": "json", "language": "all"})
    with urllib.request.urlopen(url, timeout=30) as resp:
        return json.load(resp)


def main(argv):
    for query in argv:
        print("=" * 78)
        print("Q: " + query)
        print("=" * 78)
        try:
            data = search(query)
        except Exception as exc:            # noqa: BLE001 - 查不到就报一句
            print("  !! 查询失败: %s" % exc)
            continue
        for item in data.get("results", [])[:N]:
            print("- %s" % item.get("title", "").strip())
            print("  %s" % item.get("url", ""))
            content = (item.get("content") or "").strip().replace("\n", " ")
            if content:
                print("  %s" % content[:400])
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
