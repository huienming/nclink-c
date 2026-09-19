#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""从 ARM 反汇编里把"栈上拼出来的字节数组"还原出来。

Go 编译器把 `[...]byte{...}` 这类字面量编译成一串
`mov rX, #imm` + `strb rX, [sp, #off]`（数组大时先 duffzero 清零再补非零格），
所以只要跟踪这几个寄存器的值，就能把字面量整块读回来——不用跑真机。

用法（objdump 认二进制不认 Go，所以先按 .gopclntab 切出函数）:
    python tools/site-probe/arm_bytes.py <bin> '<函数名>'

输出：每一处 `strb` 写进的 (栈偏移, 字节)，以及清零循环覆盖到的区间。
"""

import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def load_pclntab():
    spec = importlib.util.spec_from_file_location(
        "go_pclntab", os.path.join(HERE, "go_pclntab.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


LINE = re.compile(r"^\s+([0-9a-f]+):\t([0-9a-f]{8})\s+(\S+)\s*(.*)$")
STORE = re.compile(r"^(\w+),\s*\[([^\]]+)\](!?)(?:\s*,\s*#(-?\d+))?$")


def parse_number(text):
    text = text.strip()
    if text.startswith("#"):
        text = text[1:]
    if text.startswith("0x"):
        return int(text, 16)
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    return None


def simulate(asm):
    regs = {}                     # 寄存器 -> 立即数 或 ('sp', off)
    ptr = {}                      # 自增指针寄存器 -> 栈偏移
    writes = []                   # (栈偏移, 字节, 来源指令)
    zeroed = {}                   # 清零循环的首地址 -> (起点, 终点)
    pending_zero = None

    for line in asm.split("\n"):
        match = LINE.match(line)
        if not match:
            continue
        addr, _, op, rest = match.groups()
        addr = int(addr, 16)
        rest = re.sub(r"@.*$", "", rest).strip()
        args = [a.strip() for a in rest.split(",")] if rest else []

        store = STORE.match(rest) if op in ("strb", "str") else None
        if store:
            src, inner, bang, post = store.groups()
            value = regs.get(src)
            value = value & 0xFF if isinstance(value, int) else None
            base, _, off_text = [p.strip() for p in inner.partition(",")]
            off = parse_number(off_text) or 0 if off_text else 0
            if base == "sp":
                if value is not None:
                    writes.append((off, value, addr))
            else:
                where = ptr.get(base, 0) + off
                if value is not None:
                    writes.append((where, value, addr))
                if post and post.lstrip("#"):
                    ptr[base] = ptr.get(base, 0) + (parse_number("#" + post)
                                                    or 0)
                if value == 0 and pending_zero:
                    first, last = pending_zero
                    zeroed.setdefault(first, (first, last, addr))
                    if where + 1 >= last:
                        pending_zero = None
            continue
        if op == "mov" and len(args) >= 2:
            dst = args[0]
            if args[1].startswith("#"):
                regs[dst] = parse_number(args[1])
                if dst in ptr:
                    del ptr[dst]
            elif args[1].startswith("sp"):
                regs[dst] = ("sp", 0)
            elif args[1] in regs or args[1] in ptr:
                regs[dst] = regs.get(args[1])
            else:
                regs.pop(dst, None)
        elif op == "movw" and len(args) >= 2:
            regs[args[0]] = parse_number(args[1])
        elif op == "movt" and len(args) >= 2:
            old = regs.get(args[0])
            imm = parse_number(args[1])
            if isinstance(old, int) and imm is not None:
                regs[args[0]] = (old & 0xFFFF) | (imm << 16)
        elif op == "mvn" and len(args) >= 2:
            imm = parse_number(args[1])
            if imm is not None:
                regs[args[0]] = (~imm) & 0xFFFFFFFF
        elif op == "add" and len(args) >= 2:
            dst = args[0]
            if args[1] == "sp":
                off = parse_number(args[2]) if len(args) > 2 else 0
                regs[dst] = ("sp", off or 0)
            elif args[1] in regs and isinstance(regs[args[1]], tuple):
                off = parse_number(args[2]) if len(args) > 2 else 0
                regs[dst] = ("sp", regs[args[1]][1] + (off or 0))
            elif args[1] in ptr and len(args) > 2:
                ptr[dst] = ptr[args[1]] + (parse_number(args[2]) or 0)
            else:
                regs.pop(dst, None)
                ptr.pop(dst, None)
            # 清零循环：`add r1, sp, #A` .. `add r2, sp, #B` 之后成对出现
            if dst in regs and isinstance(regs[dst], tuple):
                for other, other_value in list(regs.items()):
                    if other == dst or not isinstance(other_value, tuple):
                        continue
                    first, last = regs[dst][1], other_value[1]
                    if 0 < first < last < 4096:
                        pending_zero = (first, last)
        elif op in ("ldr", "ldrb", "ldrh", "ldrsb") and args:
            regs.pop(args[0], None)
            ptr.pop(args[0], None)

    return writes, sorted(zeroed.values())


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    binary, want = argv[0], argv[1]
    gp = load_pclntab()
    blob = open(binary, "rb").read()
    pcln = gp.Pcln(blob)
    picked = [f for f in pcln.entries()
              if f[0].endswith(want) or want in f[0] or ("%#x" % f[1]) == want]
    if not picked:
        print("没找到 %s" % want)
        return 1
    name, start, end = picked[0]
    sects = gp.sections(blob)
    match = [(addr, off) for addr, off, _ in sects.values()
             if addr == pcln.text_start]
    text_off = match[0][1]
    begin = text_off + (start - pcln.text_start)
    code = blob[begin:begin + (end - start)]
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as handle:
        handle.write(code)
        path = handle.name
    try:
        asm = subprocess.run(
            ["objdump", "-D", "-b", "binary", "-m", "arm",
             "--adjust-vma=%#x" % start, path],
            capture_output=True, text=True).stdout
    finally:
        os.unlink(path)

    writes, zeroed = simulate(asm)
    print("=== %s  %#x-%#x" % (name, start, end))
    print("--- 清零循环")
    for first, last, addr in zeroed:
        print("  [%#x..%#x) = 0   (%#x)" % (first, last, addr))
    print("--- 逐字节写入")
    for off, value, addr in sorted(writes):
        print("  sp+%-4d = %02x   (%#x)" % (off, value, addr))

    # 合并成"连续区间"看字面量
    print("--- 字面量（连续区间的字节）")
    run = []
    for off, value, _ in sorted(writes):
        if run and off != run[-1][0] + 1:
            print("  %s" % describe(run))
            run = []
        run.append((off, value))
    if run:
        print("  %s" % describe(run))
    return 0


def describe(run):
    raw = bytes(value for _, value in run)
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in raw)
    return "sp+%-4d..%-4d (%3d) %s  %s" % (
        run[0][0], run[-1][0] + 1, len(raw), raw.hex(), text)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
