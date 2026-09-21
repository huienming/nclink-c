#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""从 FANUC 官方的 FOCAS 动态库里扫出每个 cnc_* 调用用的 **Cb 码**（item code）。

背景：FOCAS 的线上协议里，每个数据调用发的是一条 `func 0x21` 请求，请求里带一个或
几个"命令块"（Cb），Cb 的 `c` 字段（[6..8)）就是这个调用的数据项码 —— 例如
`cnc_actf` 是 0x24、`cnc_rdcount` 是 0x8b。这些码**不在公开头文件里**，只在库里；
本脚本就是把它们从库里读出来（和 01 册 §2.3 那套"假机床 + 报文对照"互为印证）：

    python focas_item_scan.py Fwlib64/fwlib30i64.dll [函数名...]

做法：PE 导出表给出每个 cnc_* 的入口地址；反汇编它这一段的机器码，收集紧挨着
"构造 Cb / 发请求"的 call 之前那些小立即数（`mov reg, imm` 与 `push imm`），
按出现顺序列出来。经验上 `c` 就是其中那个"上界在 0x200 以内、又不是 0/1"的常数。

不是反编译，也不保证 100%：噪声靠 `focas_sdk_probe`（真发一遍帧看 Cb 码）兜底。
"""
import re
import sys

import capstone
import pefile

# 立即数上界：Cb 的 c 字段是 u16，但实际用到的码都在 0x200 以内（0x120 是最大的
# 一个：cnc_rdtimer）。再大的常数基本都是长度/偏移，不收。
MAX_CODE = 0x200


def export_map(pe):
    out = {}
    for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if sym.name:
            out[sym.name.decode()] = sym.address
    return out


def function_body(pe, addr, limit=0x600):
    """函数的机器码：从入口往后取 limit 字节（导出符号带 size 就用 size）。"""
    for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if sym.address == addr and getattr(sym, "forwarder", None) is None:
            pass
    data = pe.get_memory_mapped_image()
    off = addr
    return data[off:off + limit]


def scan(bytes_, base_va, mode64):
    md = capstone.Cs(capstone.CS_ARCH_X86,
                     capstone.CS_MODE_64 if mode64 else capstone.CS_MODE_32)
    md.detail = False
    immediates = []
    for insn in md.disasm(bytes_, base_va):
        op = insn.op_str
        m = re.match(r"^(r|e)?[a-z0-9]+(d|w|b)?,\s*(0x[0-9a-f]+|\d+)$", op)
        if insn.mnemonic.startswith("mov") and m:
            value = int(m.group(3), 0)
            immediates.append((insn.address, insn.mnemonic, op, value))
        elif insn.mnemonic == "push" and re.match(r"^(0x[0-9a-f]+|\d+)$", op):
            immediates.append((insn.address, insn.mnemonic, op, int(op, 0)))
    return immediates


def candidates(immediates):
    """小常数按出现顺序去重，像 Cb 码的那些排前面。"""
    seen = []
    for _, _, _, value in immediates:
        if 2 <= value < MAX_CODE and value not in seen:
            seen.append(value)
    return seen


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    path = argv[0]
    want = [w.lower() for w in argv[1:]]
    pe = pefile.PE(path, fast_load=True)
    mode64 = pe.FILE_HEADER.Machine == 0x8664  # AMD64
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    if not hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        raise SystemExit("没有导出表：%s" % path)
    exports = export_map(pe)
    print("# %s：%d 个导出符号" % (path, len(exports)))
    names = sorted(n for n in exports if n.startswith("cnc_") or
                   n.startswith("pmc_"))
    if want:
        names = [n for n in names if n.lower() in want]
    for name in names:
        addr = exports[name]
        body = function_body(pe, addr)
        imm = scan(body, addr, mode64)
        cand = candidates(imm)
        print("%-28s %s" % (name, " ".join("0x%02x" % c for c in cand[:8])))
    pe.close()


if __name__ == "__main__":
    main(sys.argv[1:])
