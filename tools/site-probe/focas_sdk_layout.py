#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""问官方 SDK："应答载荷的第几个字节落到出参结构的哪一格"。

`focas_sdk_probe.exe` 只能一条一条跑，出参是一堆 hexdump，靠人眼对偏移很容易
看岔（尤其是结构体头几个字是 short 还是 long）。这一支把这件事自动化：

  1. 铺一份"每个字都不一样"的载荷（第 i 个字 = 0x1000 + i*0x101，高低字节都不重复）；
  2. 跑一次探针，把 SDK 填进出参的字节抠出来；
  3. 对出参的每一格，在载荷里**反查**这一格的值是从哪儿来的（大端/小端、16/32 位
     各试一遍），唯一命中就报出来。

    python focas_sdk_layout.py cnc_rdtofsinfo
    python focas_sdk_layout.py cnc_rdparam 0 0 8 --len 8
    python focas_sdk_layout.py --calls cnc_rdlife:1,cnc_rdmacro:0

只报"唯一命中"：一格对不上载荷里任何位置（比如 SDK 自己按语义改写过，或者
那一格本来就不是从载荷来的），就标 `?`；命中多处就标 `*`（说明载荷铺得不够
花，换一份再看）。

用法上的两个坑（都在 focas_sdk_probe.ps1 里踩过）：
  - 官方库对"数据块长度/条数"查得严，给 0 会本地回 EW_ATTRIB/EW_LENGTH 而不发帧，
    所以 `--len`/`--count` 该给的要给；
  - 探针的调用原型是**通用形状**（s1/s2/s3/s1_n/...），和官方头里那种
    `(h, ODBXXX *)` 不一定逐字对上，出参里偶尔会有几格落在别处 —— 反查能对上
    载荷的那几格才当数。
"""
import os
import re
import subprocess
import sys
import time

WORK = os.path.join(os.environ.get("TEMP", "."), "focas-probe")
PROBE = os.path.join(WORK, "focas_sdk_probe.exe")
MOCK = os.path.join(WORK, "focas_sdk_mock.py")
PORT = 8201


def payload_hex(words=32):
    """每个字都不一样：第 i 个字 = 0x1000 + i*0x101（高低字节也都不重复）。"""
    out = bytearray()
    for i in range(words):
        w = (0x1000 + i * 0x101) & 0xFFFF
        out += w.to_bytes(2, "big")
    return out.hex()


def find(value, size, payload):
    """在载荷里反查这个值：返回 ["off:BE16", ...]。"""
    hits = []
    for off in range(0, len(payload) - size + 1):
        for tag, order in (("BE", "big"), ("LE", "little")):
            if int.from_bytes(payload[off:off + size], order) == value:
                hits.append("%s%s@%d" % (tag, size * 8, off))
    return hits


def run_one(call, args, extra, timeout=25):
    log = os.path.join(WORK, "layout-mock.log")
    with open(log, "wb") as fp:
        mock = subprocess.Popen(
            [sys.executable, "-u", MOCK, str(PORT), "--size", "0x300",
             "--payload", extra["payload"]],
            stdout=fp, stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    time.sleep(1.2)
    try:
        argv = [PROBE, "127.0.0.1", str(PORT), call] + [str(a) for a in args]
        if extra.get("len"):
            argv += ["--len", str(extra["len"])]
        if extra.get("count"):
            argv += ["--count", str(extra["count"])]
        out = subprocess.run(argv, capture_output=True, text=True,
                             timeout=timeout).stdout
    finally:
        time.sleep(0.2)
        mock.terminate()
    return out, open(log, encoding="utf-8", errors="replace").read()


def parse_buf(text):
    """把探针打印的 ` 0000  xx xx .. | ascii` 抠成 bytes。"""
    buf = bytearray()
    for line in text.splitlines():
        m = re.match(r"^  ([0-9a-f]{4})  ((?:[0-9a-f]{2} ){1,16})", line)
        if not m:
            continue
        if int(m.group(1), 16) != len(buf):
            continue          # 只要从 0 开始那一段（出参结构）
        buf += bytes(int(b, 16) for b in m.group(2).split())
    return bytes(buf)


def report(call, args, out, log, payload):
    rc = re.search(r"rc = (-?\d+)", out)
    cb = [l.strip() for l in log.splitlines() if l.strip().startswith("Cb ")]
    buf = parse_buf(out)
    print("== %s %s   rc=%s" % (call, " ".join(str(a) for a in args),
                                rc.group(1) if rc else "?"))
    # 握手期固定这三条，和被测调用无关：0x0e/0x26f0（能力）与 0x89（轴表）
    own = [c for c in cb
           if not ("code=0x0e (14) arg0=0x000026f0" in c or "code=0x89 (137)" in c)]
    for c in own:
        print("   Cb %s" % c.split("  0")[0].replace("Cb ", ""))
    for size in (2, 4):
        for off in range(0, min(len(buf), 40) - size + 1, size):
            value = int.from_bytes(buf[off:off + size], "little")
            hits = find(value, size, payload)
            print("   out[%2d..%d) = 0x%0*x  <- %s" % (
                off, off + size, size * 2, value,
                hits[0] if len(hits) == 1 else ("?" if not hits else
                                                 "*(%s)" % ",".join(hits[:4]))))


def main(argv):
    calls = []
    extra = {"payload": payload_hex(), "len": 0, "count": 0}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--len":
            i += 1
            extra["len"] = argv[i]
        elif a == "--payload":
            i += 1
            extra["payload"] = argv[i]
        elif a == "--count":
            i += 1
            extra["count"] = argv[i]
        elif a == "--calls":
            i += 1
            for spec in argv[i].split(","):
                parts = spec.split(":")
                calls.append((parts[0], parts[1].split("+") if len(parts) > 1
                              and parts[1] else []))
        elif not calls:
            calls.append((a, []))
        else:
            calls[-1][1].append(a)
        i += 1
    if not calls:
        raise SystemExit(__doc__)
    for call, args in calls:
        out, log = run_one(call, args, extra)
        report(call, args, out, log, bytes.fromhex(extra["payload"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
