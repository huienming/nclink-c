#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""按虚拟地址看 ELF 里的字节：把字面量池（literal pool）里的指针/长度解出来。

ARM 代码里 `ldr r0, [pc, #NNN]` 拿到的是**字面量池里的一格**，格里才是字符串
指针（Go 的字符串是 (ptr, len) 一对）。反汇编只给到池子的地址，所以需要这个
脚本把"池子地址 → 池中内容 → 指向的字符串"一次走完。

    python tools/site-probe/elf_vaddr.py <bin> --words 0x64852c 4
    python tools/site-probe/elf_vaddr.py <bin> --str 0x6f1a20
    python tools/site-probe/elf_vaddr.py <bin> --gostr 0x64852c

`--gostr` 假定给的是"一格里放着 (ptr,len)"的池子地址，直接把字符串打出来。
"""

import struct
import sys


def load_sections(blob):
    if blob[:4] != b"\x7fELF":
        raise SystemExit("不是 ELF")
    is64 = blob[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", blob, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", blob, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    raw = []
    for index in range(shnum):
        base = shoff + index * shentsize
        if is64:
            name, _, _, addr, offset, size = struct.unpack_from("<IIQQQQ", blob, base)
        else:
            name, _, _, addr, offset, size = struct.unpack_from("<IIIIII", blob, base)
        raw.append((name, addr, offset, size))
    strtab = raw[shstrndx][2]

    def name_of(off):
        end = blob.index(b"\x00", strtab + off)
        return blob[strtab + off:end].decode("latin-1")

    return [(name_of(n), a, o, s) for n, a, o, s in raw]


def to_offset(sects, vaddr):
    for name, addr, offset, size in sects:
        if addr <= vaddr < addr + size:
            return offset + (vaddr - addr)
    raise SystemExit("虚拟地址 %#x 不在任何段里" % vaddr)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    blob = open(argv[0], "rb").read()
    sects = load_sections(blob)
    if "--words" in argv:
        vaddr = int(argv[argv.index("--words") + 1], 16) if argv[
            argv.index("--words") + 1].startswith("0x") else int(
                argv[argv.index("--words") + 1])
        count = int(argv[argv.index("--words") + 2]) if len(
            argv) > argv.index("--words") + 2 else 4
        off = to_offset(sects, vaddr)
        for index in range(count):
            word, = struct.unpack_from("<I", blob, off + index * 4)
            print("  [%#x] = %#010x" % (vaddr + index * 4, word))
        return 0
    if "--str" in argv:
        text = argv[argv.index("--str") + 1]
        vaddr = int(text, 16) if text.startswith("0x") else int(text)
        off = to_offset(sects, vaddr)
        end = blob.index(b"\x00", off)
        print(blob[off:end].decode("latin-1"))
        return 0
    if "--gostr" in argv:
        text = argv[argv.index("--gostr") + 1]
        vaddr = int(text, 16) if text.startswith("0x") else int(text)
        off = to_offset(sects, vaddr)
        ptr, length = struct.unpack_from("<II", blob, off)
        if length > 4096:
            print("长度 %d 不像字符串（ptr=%#x）" % (length, ptr))
            return 1
        soff = to_offset(sects, ptr)
        print("%r" % blob[soff:soff + length].decode("latin-1"))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
