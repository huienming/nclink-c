# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""从新代 .NET 程序集里抠"业务方法 → uFuncID / CmdID"（`syntec_re` 那套的干净版）。

10 册 §10.3 的"下一步"就是这件事。老的 `syntec_re/funcids.py` 静默吞掉了所有异常，
所以看着像"没有命中"。这个版本把统计与错误都打出来。

    python tools/site-probe/syntec_funcids.py <dll> [<dll> ...] [--dump-switch]

约定：
    ldc.i4* <N> ... stfld uFuncID     → N 就是命令号
    switch <表>                        → 打印分派表里的 case 值（`--dump-switch`）
"""
import os
import sys

import dnfile
from dncil.cil.body import CilMethodBody
from dncil.cil.body.reader import CilMethodBodyReaderBytes

WATCH = ("uFuncID", "CmdID", "IHeader", "uSerial")


def s(x):
    return str(getattr(x, "value", x))


def const_of(insn):
    name = insn.opcode.name.lower()
    if name.startswith("ldc.i4.s") or (name.startswith("ldc.i4.") and name != "ldc.i4."):
        tail = name.split(".")[-1]
        if tail == "m1":
            return -1
        try:
            return int(tail)
        except ValueError:
            return None
    if name in ("ldc.i4", "ldc.i4.s"):
        try:
            return int(insn.operand)
        except (TypeError, ValueError):
            return None
    return None


def field_names(pe):
    out = {}
    for rid, r in enumerate(pe.net.mdtables.Field.rows, start=1):
        row = getattr(r, "row", r)
        out[rid] = s(row.Name)
    return out


def dump_il(pe, substr):
    """打印名字里含 substr 的方法的完整 IL（switch 的 case 值要靠它算）。"""
    fields = field_names(pe)
    for t in pe.net.mdtables.TypeDef.rows:
        trow = getattr(t, "row", t)
        tname = "%s.%s" % (s(trow.TypeNamespace), s(trow.TypeName))
        for m in trow.MethodList or []:
            row = getattr(m, "row", m)
            full = tname + "." + s(row.Name)
            if substr not in full or not row.Rva:
                continue
            print("======== %s" % full)
            try:
                body = CilMethodBody(CilMethodBodyReaderBytes(pe.get_data(row.Rva, 65536)))
            except Exception as exc:                 # noqa: BLE001
                print("   !! %s" % exc)
                continue
            for insn in body.instructions:
                name = insn.opcode.name.lower()
                extra = ""
                if name == "stfld":
                    rid = getattr(insn.operand, "rid", None)
                    extra = fields.get(rid, "") if rid else ""
                elif name.startswith("call"):
                    extra = str(insn.operand)[:70]
                elif name == "switch":
                    ops = getattr(insn, "operand", None) or []
                    extra = "%d 个分支" % len(ops)
                elif insn.operand is not None and name.startswith("ldc"):
                    extra = str(insn.operand)
                print("  %-8s %-22s %s" % (hex(insn.offset), name, extra))


def scan(path, dump_switch):
    pe = dnfile.dnPE(path)
    fields = field_names(pe)
    print("######## %s" % os.path.basename(path))
    print("  字段数 %d" % len(fields))

    n_method, n_body, n_err = 0, 0, 0
    errors = {}
    hits = 0

    for t in pe.net.mdtables.TypeDef.rows:
        trow = getattr(t, "row", t)
        tname = "%s.%s" % (s(trow.TypeNamespace), s(trow.TypeName))
        for m in trow.MethodList or []:
            row = getattr(m, "row", m)
            n_method += 1
            if not row.Rva:
                continue
            try:
                body = CilMethodBody(CilMethodBodyReaderBytes(pe.get_data(row.Rva, 16384)))
                n_body += 1
            except Exception as exc:                     # noqa: BLE001
                n_err += 1
                errors.setdefault(type(exc).__name__ + ": " + str(exc)[:60], 0)
                errors[type(exc).__name__ + ": " + str(exc)[:60]] += 1
                continue

            instrs = list(body.instructions)
            last = None
            for i, insn in enumerate(instrs):
                c = const_of(insn)
                if c is not None:
                    last = c
                    continue
                if insn.opcode.name.lower() == "stfld":
                    tok = getattr(insn, "operand", None)
                    rid = getattr(tok, "rid", None)
                    fname = fields.get(rid, "") if rid else ""
                    if fname in WATCH and last is not None:
                        print("  %-46s  %s = %d (0x%x)"
                              % (tname + "." + s(row.Name), fname, last, last & 0xFFFFFFFF))
                        hits += 1
                        last = None
                elif dump_switch and insn.opcode.name.lower() == "switch":
                    # `switch (cmd - base)` 是 IL 的常见形状：看前一条是不是 `sub`
                    ops = getattr(insn, "operand", None) or []
                    n = len(ops)
                    base = None
                    if i >= 2 and instrs[i - 1].opcode.name.lower() == "sub":
                        base = const_of(instrs[i - 2])
                    if base is not None:
                        print("  switch %-58s %2d case -> cmdID %d..%d"
                              % (tname + "." + s(row.Name), n, base, base + n - 1))
                    else:
                        print("  switch %-58s %2d case -> base 不明" % (tname + "." + s(row.Name), n))
                    hits += 1

    print("  方法 %d / 有实体 %d / 解析失败 %d / 命中 %d" % (n_method, n_body, n_err, hits))
    for k, v in sorted(errors.items(), key=lambda kv: -kv[1])[:5]:
        print("    错误 x%d  %s" % (v, k))
    return hits


def main(argv):
    dump_switch = "--dump-switch" in argv
    il = None
    rest = []
    for a in argv:
        if a == "--dump-switch":
            continue
        if a.startswith("--il="):
            il = a[5:]
            continue
        rest.append(a)
    if il is not None:
        for p in rest:
            if os.path.exists(p) and p.lower().endswith((".dll", ".exe")):
                dump_il(dnfile.dnPE(p), il)
            else:
                print("!! 没有 %s" % p)
        return 0
    total = 0
    for p in rest:
        if os.path.exists(p):
            total += scan(p, dump_switch)
        else:
            print("!! 没有 %s" % p)
    print("\n合计命中 %d" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
