#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""按符号名反汇编 ELF（ARM/Thumb 自动认），Windows 上没 objdump 也能用。

交付包里那份 `libfwlib32.so` 是 **ARM** 的（现场网关用的真·以太网客户端），
`focas_dis.sh` 那套要 Linux + objdump + 挂载镜像；这个脚本直接用 capstone，
在 Windows 上就能读——上一轮"伺服延迟量每轴 8 字节"是从 **x86 的 HSSB 库**
（`fwlibNCG.dll`）反出来的，这次换 ARM 的**以太网**库来对一遍。

用法：

    python tools/site-probe/elf_dis.py <elf> --syms cnc_          # 列符号
    python tools/site-probe/elf_dis.py <elf> cnc_srvdelay [条数]   # 反汇编
    python tools/site-probe/elf_dis.py <elf> 0x1b534 [条数]        # 按地址
    python tools/site-probe/elf_dis.py <elf> 0x1b534 200 --pool    # 附带字面量池
"""
import importlib.util
import os
import struct
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))


def load_elf_vaddr():
    spec = importlib.util.spec_from_file_location(
        "elf_vaddr", os.path.join(HERE, "elf_vaddr.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def symbols(blob, sects, mode64):
    """(名字 → (地址, 是否 Thumb))，只收 FUNC 与 OBJECT 里带名字的。"""
    out = {}
    for name, addr, offset, size in sects:
        if name not in (".dynsym", ".symtab"):
            continue
        link = None
        for other_name, _, other_offset, _ in sects:
            if other_name == ".dynstr" or other_name == ".strtab":
                if other_name == (".dynstr" if name == ".dynsym" else ".strtab"):
                    link = other_offset
        if link is None:
            continue
        count = size // (24 if mode64 else 16)
        for index in range(count):
            base = offset + index * (24 if mode64 else 16)
            if mode64:
                st_name, info, _, st_value, st_size = struct.unpack_from(
                    "<IBBHQ", blob, base)
            else:
                st_name, st_value, st_size, info, _ = struct.unpack_from(
                    "<IIIBB", blob, base)
            if st_name == 0 or st_value == 0:
                continue
            end = blob.index(b"\x00", link + st_name)
            text = blob[link + st_name:end].decode("latin-1")
            thumb = (st_value & 1) != 0 and not mode64
            out[text] = (st_value & ~1, thumb)
    return out


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path, want = argv[0], argv[1]
    count = 80
    if want != "--syms" and len(argv) > 2 and not argv[2].startswith("--"):
        count = int(argv[2], 0)
    show_pool = "--pool" in argv

    blob = open(path, "rb").read()
    elf_vaddr = load_elf_vaddr()
    sects = elf_vaddr.load_sections(blob)
    mode64 = blob[4] == 2
    machine = struct.unpack_from("<H", blob, 0x12)[0]
    syms = symbols(blob, sects, mode64)
    print("# %s  %d 个符号  machine=0x%x" % (path, len(syms), machine))

    if want == "--syms":
        keyword = argv[2] if len(argv) > 2 else ""
        for name in sorted(n for n in syms if keyword in n):
            print("  %-52s %#x%s" % (name, syms[name][0],
                                     " (thumb)" if syms[name][1] else ""))
        return 0

    if want in syms:
        vaddr, thumb = syms[want]
        print("# %s @ %#x%s" % (want, vaddr, " thumb" if thumb else ""))
    else:
        vaddr = int(want, 0)
        thumb = False
        print("# 地址 %#x" % vaddr)

    if machine == 0x28:  # EM_ARM
        mode = capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM
        md = capstone.Cs(capstone.CS_ARCH_ARM,
                         mode | capstone.CS_MODE_LITTLE_ENDIAN)
    elif machine == 0x03:  # EM_386（Fwlib/Linux/x86 那份）
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    elif machine == 0x3E:  # EM_X86_64
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    else:
        raise SystemExit("不认这个 machine=0x%x（ARM/Thumb、x86、x64 都行）"
                         % machine)
    offset = elf_vaddr.to_offset(sects, vaddr)
    for index, insn in enumerate(md.disasm(blob[offset:], vaddr)):
        if index >= count:
            break
        note = ""
        if show_pool and insn.mnemonic.startswith("ldr") and "pc" in insn.op_str:
            note = "   ; 字面量池"
        print("%08x  %-34s %s" % (insn.address,
                                  insn.mnemonic + " " + insn.op_str, note))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
