#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""从 Go 二进制里认出函数：解析 .gopclntab，列出/导出函数的地址范围。

Go 二进制的符号表就算 strip 了，`.gopclntab` 也还在——里面是
`functab`（entryoff, funcoff）+ 函数名表，于是**每个函数的名字、起止地址**
都能还原。要读某段被 strip 掉的代码（比如网关的
`hp2x/protocols/siemens/plc/s7v2.(*S7).Execution`），先在这里拿到它的
地址范围，再交给 objdump。

    python tools/site-probe/go_pclntab.py <bin> --list s7v2
    python tools/site-probe/go_pclntab.py <bin> --dump '(*S7).Execution' > code.bin
    # 然后（objdump 认二进制，不认 Go）：
    #   objdump -D -b binary -m arm --adjust-vma=<entry> code.bin

只支持 Go 1.18+ 的 pclntab（magic 0xFFFFFFF0/F1），且假定 ELF32/ELF64
小端——现场这两个包（hp2x_box200、lib*.so）正是如此。
"""

import re
import struct
import sys


def sections(blob):
    """返回 {名字: (addr, offset, size)}；只处理小端 ELF。"""
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
    strtab_off = raw[shstrndx][2]

    def name_of(off):
        end = blob.index(b"\x00", strtab_off + off)
        return blob[strtab_off + off:end].decode("latin-1")

    return {name_of(n): (a, o, s) for n, a, o, s in raw}


class Pcln:
    def __init__(self, blob):
        sect = sections(blob)
        key = ".gopclntab" if ".gopclntab" in sect else ".data.rel.ro.gopclntab"
        addr, off, size = sect[key]
        self.addr = addr                 # 段虚拟地址（entry 相对 textStart）
        self.off = off
        self.data = blob[off:off + size]
        magic = struct.unpack_from("<I", self.data, 0)[0]
        if magic & 0xFFFFFFF0 != 0xFFFFFFF0:
            raise SystemExit("magic=%#x：不是 1.18+ 的 pclntab" % magic)
        ptr = self.data[7]
        self.ptr = ptr
        unit = "Q" if ptr == 8 else "I"
        fmt = "<" + unit
        pos = 8
        self.nfunc, self.nfiles = struct.unpack_from("<" + unit * 2, self.data, pos)
        pos += 2 * ptr
        self.text_start, = struct.unpack_from(fmt, self.data, pos)
        pos += ptr
        (name_off, cu_off, file_off, pctab_off,
         pcln_off) = struct.unpack_from("<" + unit * 5, self.data, pos)
        self.names = self.data[name_off:]
        self.pcln = pcln_off
        self.table = self.data[pcln_off:]

    def func_name(self, funcoff):
        nameoff = struct.unpack_from("<i", self.table, funcoff + 4)[0]
        end = self.names.index(b"\x00", nameoff)
        return self.names[nameoff:end].decode("latin-1")

    def entries(self):
        """(名字, 起始 vaddr, 结束 vaddr)。"""
        out = []
        for index in range(self.nfunc):
            entryoff, funcoff = struct.unpack_from("<II", self.table, index * 8)
            name = self.func_name(funcoff)
            out.append((name, self.text_start + entryoff, index))
        out.sort(key=lambda item: item[1])
        result = []
        for index, (name, start, _) in enumerate(out):
            end = out[index + 1][1] if index + 1 < len(out) else start
            result.append((name, start, end))
        return result


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    blob = open(argv[0], "rb").read()
    pcln = Pcln(blob)
    funcs = pcln.entries()

    if "--list" in argv:
        pattern = re.compile(argv[argv.index("--list") + 1])
        for name, start, end in funcs:
            if pattern.search(name):
                print("%#010x - %#010x  %5d  %s" % (start, end, end - start, name))
        return 0

    if "--dump" in argv:
        want = argv[argv.index("--dump") + 1]
        picked = [f for f in funcs if f[0].endswith(want) or want in f[0]
                  or ("%#x" % f[1]) == want]
        if not picked:
            return 1
        name, start, end = picked[0]
        # functab 的 entryoff 是相对 textStart 的；把 textStart 映射回文件偏移
        match = [(addr, off) for addr, off, _ in sections(blob).values()
                 if addr == pcln.text_start]
        if not match:
            print("找不到 textStart=%#x 对应的段" % pcln.text_start, file=sys.stderr)
            return 1
        _, text_off = match[0]
        begin = text_off + (start - pcln.text_start)
        with open(argv[0], "rb") as handle:
            handle.seek(begin)
            code = handle.read(end - start)
        if "--out" in argv:
            with open(argv[argv.index("--out") + 1], "wb") as handle:
                handle.write(code)
            print("%s %#x %#x -> %s" % (name, start, end,
                                        argv[argv.index("--out") + 1]))
        else:
            sys.stdout.write("%s %#x %#x\n" % (name, start, end))
            sys.stdout.flush()
            sys.stdout.buffer.write(code)
        return 0

    print("%d 个函数，textStart=%#x" % (pcln.nfunc, pcln.text_start))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
