#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""把官方 FOCAS 动态库里的**一段**反汇编出来，按 VA 打，顺带把 call 的目标标成名字。

`focas_item_scan.py` 只回答"这个调用发的 Cb 码是多少"（扫立即数），回答不了
"应答怎么切" —— 那要看 shell 转进内部函数之后那段代码：长度校验（EW_LENGTH）
的常数、拷贝循环的步长，都是在这段里。用法：

    python focas_dis_range.py <dll> <rva-hex> [条数]

32 位库（NCGuide 随包那份）地址是"镜像基址 + RVA"；`Call` 后面括号里给出
被调函数的 RVA（对上导出表就是函数名）。反汇编不是反编译：拷过来的是机器码，
结论要拿假机床/真机实测再对一遍。
"""
import sys

import capstone
import pefile


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[0]
    rva = int(argv[1], 0)
    count = int(argv[2], 0) if len(argv) > 2 else 120

    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    mode64 = pe.FILE_HEADER.Machine == 0x8664
    exports = {}
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if sym.name:
                exports[base + sym.address] = sym.name.decode()

    data = pe.get_memory_mapped_image()
    # 直接给基址 + RVA；给成 VA 也能用（大于基址就当 VA）
    va = rva if rva >= base else base + rva
    off = va - base
    md = capstone.Cs(capstone.CS_ARCH_X86,
                     capstone.CS_MODE_64 if mode64 else capstone.CS_MODE_32)
    print("# %s  基址 %#x  RVA %#x  VA %#x" % (path, base, va - base, va))
    for i, insn in enumerate(md.disasm(data[off:], va)):
        if i >= count:
            break
        note = ""
        if insn.mnemonic == "call":
            note = exports.get(insn.address + insn.size + 0, "")
            try:
                target = int(insn.op_str, 16)
                note = exports.get(target, "") or "sub_%x" % target
            except ValueError:
                note = "<indirect>"
        print("%08x  %-24s  %s" % (insn.address, insn.mnemonic + " " +
                                   insn.op_str, note))
    pe.close()


if __name__ == "__main__":
    main(sys.argv[1:])
