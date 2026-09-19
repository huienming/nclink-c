#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 看一个 Go 函数的"判定逻辑"：把 memequal 的比较都揪出来，再把字面量池的
# 内容列出来（池子里一格是 4 字节，Go 的字符串/类型/常量都从这里拿）。
#
#   sh /work/arm_analyze.sh '(*S7).Mode'
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

BIN=/hp2x/hp2x_box200
name="$1"
line=$(python3 /work/go_pclntab.py "$BIN" --dump "$name" --out /tmp/fn.bin)
start=$(echo "$line" | awk '{print $2}')
end=$(echo "$line" | awk '{print $3}')
objdump -D -b binary -m arm --adjust-vma="$start" /tmp/fn.bin >/tmp/fn.asm

echo "=== $line"
echo "--- memequal（0x132b8）前后的比较"
grep -n -B 10 '0x132b8' /tmp/fn.asm | head -140
echo "--- 字面量池（[pc, #N] 指到的格子，一格 4 字节）"
python3 - "$start" "$end" <<'PY2'
import re
import sys

start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
lines = open("/tmp/fn.asm").read().split("\n")
refs = {}
for index, line in enumerate(lines):
    match = re.search(r"\[pc, #(\d+)\]\s*@\s*(0x[0-9a-f]+)", line)
    if match:
        addr = int(match.group(2), 16)
        if start <= addr < end:
            refs.setdefault(addr, []).append(index)
for addr in sorted(refs):
    print("  %#x 被引用于：" % addr)
    for index in refs[addr]:
        for line in lines[index:index + 3]:
            print("    " + line.strip())
        print("    ---")
PY2
echo "--- 池中内容"
python3 - "$start" "$end" <<'PY'
import re
import subprocess
import sys

start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
refs = set()
for line in open("/tmp/fn.asm"):
    match = re.search(r"\[pc, #(\d+)\]\s*@\s*(0x[0-9a-f]+)", line)
    if match:
        refs.add(int(match.group(2), 16))
for addr in sorted(refs):
    if not (start <= addr < end):
        continue
    out = subprocess.run(
        ["python3", "/work/elf_vaddr.py", "/hp2x/hp2x_box200",
         "--words", hex(addr), "2"], capture_output=True, text=True).stdout
    words = [int(line.split("=")[1].strip(), 16)
             for line in out.strip().split("\n") if "=" in line]
    print("  %#x -> %s" % (addr, out.strip().replace("\n", "  ")))
    if len(words) == 2:
        ptr, length = words
        if 0 < length <= 64:                     # (ptr, len) 像是个字符串
            text = subprocess.run(
                ["python3", "/work/elf_vaddr.py", "/hp2x/hp2x_box200",
                 "--str", hex(ptr)], capture_output=True, text=True).stdout
            print("      ^ (ptr=%#x, len=%d) = %r" % (ptr, length, text[:64]))
        elif 0 < ptr <= 64:
            pass
        else:                                    # 单看 ptr 是不是字符串
            text = subprocess.run(
                ["python3", "/work/elf_vaddr.py", "/hp2x/hp2x_box200",
                 "--str", hex(ptr)], capture_output=True, text=True).stdout
            text = text.strip()
            if text and len(text) <= 32 and all(32 <= ord(c) < 127
                                                for c in text):
                print("      ^ 单看第一个字 = %r" % text)
PY
