#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""跑一个 Windows 程序，盯住它的第一次异常，把"怎么死的"打出来。

这台机器上没装 cdb/windbg（只有 VS BuildTools），也没管理员权限去开 WER 的
LocalDumps —— 所以自己写一个最小的调试器：`CreateProcess` 带
`DEBUG_ONLY_THIS_PROCESS` 起目标，收 `DEBUG_EVENT`，在
`EXCEPTION_DEBUG_INFO` 上把异常码、异常地址、出错线程的 `Rip/Rsp/Rbp`
以及**栈上那些落在已知模块里的地址**（= 近似的调用链）打出来。

    python win_minidbg.py "C:\\path\\to\\App.exe" [参数...]
    python win_minidbg.py --cwd <目录> --timeout 60 <exe> [参数...]

用途就是本仓库常遇到的那种问题：NCGuide（Simbase.exe）起来十几秒后**静默退出**，
事件日志里只有一句 "APPCRASH ... ns.dll/MSVCR80.dll"，看不出是谁调坏的。有了栈上
那串"模块+偏移"，就能回到磁盘上把那几条指令反汇编出来（配合
`focas_dis_range.py`），看它到底在跟哪个参数较劲。

只在确认"程序真死了"时才报：不是崩溃（自己正常退出）就只说退出码。
"""
import ctypes
import ctypes.wintypes as w
import os
import struct
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x00000002
CREATE_NEW_CONSOLE = 0x00000010
INFINITE = 0xFFFFFFFF
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001

EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6

EXCEPTION_ACCESS_VIOLATION = 0xC0000005
EXCEPTION_DATATYPE_MISALIGNMENT = 0x80000002
EXCEPTION_ILLEGAL_INSTRUCTION = 0xC000001D
EXCEPTION_INT_DIVIDE_BY_ZERO = 0xC0000094
EXCEPTION_STACK_OVERFLOW = 0xC00000FD
EXCEPTION_PRIV_INSTRUCTION = 0xC0000096
EXCEPTION_BREAKPOINT = 0x80000003

CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2
CONTEXT_SEGMENTS = CONTEXT_AMD64 | 0x4
CONTEXT_FLOATING_POINT = CONTEXT_AMD64 | 0x8
CONTEXT_FULL = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT

BREAK_NAMES = {
    EXCEPTION_ACCESS_VIOLATION: "ACCESS_VIOLATION",
    EXCEPTION_DATATYPE_MISALIGNMENT: "DATATYPE_MISALIGNMENT",
    EXCEPTION_ILLEGAL_INSTRUCTION: "ILLEGAL_INSTRUCTION",
    EXCEPTION_INT_DIVIDE_BY_ZERO: "INT_DIVIDE_BY_ZERO",
    EXCEPTION_STACK_OVERFLOW: "STACK_OVERFLOW",
    EXCEPTION_PRIV_INSTRUCTION: "PRIV_INSTRUCTION",
    EXCEPTION_BREAKPOINT: "BREAKPOINT",
}


class STARTUPINFOA(ctypes.Structure):
    _fields_ = [("cb", w.DWORD), ("lpReserved", w.LPSTR),
                ("lpDesktop", w.LPSTR), ("lpTitle", w.LPSTR),
                ("dwX", w.DWORD), ("dwY", w.DWORD),
                ("dwXSize", w.DWORD), ("dwYSize", w.DWORD),
                ("dwXCountChars", w.DWORD), ("dwYCountChars", w.DWORD),
                ("dwFillAttribute", w.DWORD), ("dwFlags", w.DWORD),
                ("wShowWindow", w.WORD), ("cbReserved2", w.WORD),
                ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
                ("hStdInput", w.HANDLE), ("hStdOutput", w.HANDLE),
                ("hStdError", w.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE),
                ("dwProcessId", w.DWORD), ("dwThreadId", w.DWORD)]


class DEBUG_EVENT(ctypes.Structure):
    """dwDebugEventCode + pid + tid + union。

    坑：x64 下那个 union 里有指针 → **8 字节对齐**，所以它从偏移 **16** 开始（不是
    12）。按 12 定义的话读出来的"异常码/地址"全是错位的（第一版就踩了这个，读到的
    "地址"是错开 4 字节的值）。
    """
    _fields_ = [("dwDebugEventCode", w.DWORD), ("dwProcessId", w.DWORD),
                ("dwThreadId", w.DWORD), ("pad", w.DWORD),
                ("u", ctypes.c_byte * 256)]


class WOW64_CONTEXT(ctypes.Structure):
    """32 位进程的 CONTEXT（`Wow64GetThreadContext` 用），只要 Eip/Ebp/Esp。"""
    _fields_ = [("buf", ctypes.c_byte * 0x2CC)]


# WOW64_CONTEXT 里这三格的偏移（winnt.h 的 i386 CONTEXT 布局）
W32_EIP = 0xB8
W32_EBP = 0xB4
W32_ESP = 0xC4
WOW64_CONTEXT_FULL = 0x00010007


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64at(blob, off):
    return struct.unpack_from("<Q", blob, off)[0]


def sym(off):
    return "%#x" % off


class Dbg(object):
    def __init__(self, exe, args, cwd):
        self.mods = []          # [(base, size, path)]
        self.pi = PROCESS_INFORMATION()
        si = STARTUPINFOA()
        si.cb = ctypes.sizeof(si)
        cmd = subprocess_cmdline(exe, args)
        ok = kernel32.CreateProcessA(
            exe.encode("mbcs") if exe else None, cmd.encode("mbcs"),
            None, None, False,
            DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE,
            None, cwd.encode("mbcs") if cwd else None,
            ctypes.byref(si), ctypes.byref(self.pi))
        if not ok:
            raise SystemExit("CreateProcess 失败：%d" % ctypes.get_last_error())
        self.pid = self.pi.dwProcessId
        wow = ctypes.c_int(0)
        kernel32.IsWow64Process(self.pi.hProcess, ctypes.byref(wow))
        self.is32 = bool(wow.value)

    def module_of(self, addr):
        for base, size, path in self.mods:
            if base <= addr < base + size:
                return path, addr - base
        return None, 0

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        ok = kernel32.ReadProcessMemory(self.pi.hProcess, ctypes.c_void_p(addr),
                                        buf, size, ctypes.byref(got))
        return buf.raw[:got.value] if ok else b""

    def add_module(self, base, path):
        size = 0
        try:
            with open(path, "rb") as fp:
                head = fp.read(0x400)
            e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
            size = struct.unpack_from("<I", head, e_lfanew + 0x50)[0]
        except OSError:
            pass
        self.mods.append((base, size or 0x1000, path))

    def stack_scan(self, ctx, span=0x4000):
        """把栈上"落在已知模块里"的 8 字节值挑出来，当近似的调用链。"""
        if self.is32:
            sp, bp = u32(ctx, W32_ESP), u32(ctx, W32_EBP)
            step, fmt = 4, "<I"
        else:
            sp, bp = u64(ctx, 152), u64(ctx, 160)
            step, fmt = 8, "<Q"
        dump = self.read(sp, span)
        out = []
        for i in range(0, len(dump) - step + 1, step):
            value = struct.unpack_from(fmt, dump, i)[0]
            path, off = self.module_of(value)
            if path is not None:
                out.append((sp + i, path, off))
        return sp, bp, out

    def report_exception(self, info):
        code = u32(info, 0)
        addr = u64(info, 16)
        nparam = u32(info, 24)
        params = [u64(info, 32 + 8 * i) for i in range(min(nparam, 15))]
        print("异常 %#x %s  @ %#x" % (code, BREAK_NAMES.get(code, "?"), addr))
        pth, off = self.module_of(addr)
        print("   出错模块：%s+%#x" % (os.path.basename(pth) if pth else "?", off))
        if code == EXCEPTION_ACCESS_VIOLATION and len(params) >= 2:
            how = {0: "读", 1: "写", 8: "执行"}.get(params[0], str(params[0]))
            print("   访问方式：%s 地址 %#x" % (how, params[1]))

    def report_context(self):
        if self.is32:
            # 32 位进程：必须用 Wow64GetThreadContext，64 位 Python 的
            # GetThreadContext 会回"原生(x64)上下文"，读出来的 Eip 是垃圾。
            buf = ctypes.create_string_buffer(0x2CC)
            ctypes.memset(buf, 0, 0x2CC)
            struct.pack_into("<I", buf, 0, WOW64_CONTEXT_FULL)
            got = kernel32.Wow64GetThreadContext(self.pi.hThread, buf)
            if not got:
                print("   Wow64GetThreadContext 失败：%d"
                      % ctypes.get_last_error())
                return None
            rip = u32(buf, W32_EIP)
        else:
            buf = ctypes.create_string_buffer(0x500)
            ctypes.memset(buf, 0, 0x500)
            struct.pack_into("<I", buf, 48, CONTEXT_FULL)
            if not kernel32.GetThreadContext(self.pi.hThread, buf):
                print("   GetThreadContext 失败：%d" % ctypes.get_last_error())
                return None
            rip = u64(buf, 248)
        pth, off = self.module_of(rip)
        rsp, rbp, frames = self.stack_scan(buf)
        print("   指令指针 %#x (%s+%#x)  Sp=%#x  Bp=%#x"
              % (rip, os.path.basename(pth) if pth else "?", off, rsp, rbp))
        print("   栈上的返回地址（模块+偏移，靠近栈底的越靠外层）：")
        seen = 0
        for slot, p, o in frames:
            print("     [%#x] %s+%#x" % (slot, os.path.basename(p), o))
            seen += 1
            if seen >= 24:
                break
        if seen == 0:
            print("     （一个都没有）")
        return buf

    def loop(self, timeout, first_break_only=True):
        deadline = time.time() + timeout
        started = False
        while time.time() < deadline:
            ev = DEBUG_EVENT()
            if not kernel32.WaitForDebugEvent(ctypes.byref(ev), 1000):
                continue
            code = ev.dwDebugEventCode
            info = ctypes.string_at(ctypes.byref(ev.u), 208)
            cont = DBG_CONTINUE
            if code == CREATE_PROCESS_DEBUG_EVENT:
                base = u64(info, 24)
                self.add_module(base, "self")
                started = True
            elif code == LOAD_DLL_DEBUG_EVENT:
                base = u64(info, 8)
                name = self.dll_name(base, info)
                self.add_module(base, name)
            elif code == EXCEPTION_DEBUG_EVENT:
                exc_code = u32(info, 0)
                nparam = u32(info, 24)
                first_chance = u32(info, 152)
                if exc_code in (EXCEPTION_BREAKPOINT, 0x4000001F):
                    # 调试断点 / WOW64 断点：正常现象，直接放过
                    cont = DBG_CONTINUE
                elif first_chance:
                    # 一次异常：交给程序自己的 SEH/CLR 处理（WER 只记二次异常）
                    addr = u64(info, 16)
                    pth, off = self.module_of(addr)
                    print("   一次异常 %#x @ %s+%#x（交给程序自己处理）"
                          % (exc_code,
                             os.path.basename(pth) if pth else "?", off))
                    cont = DBG_EXCEPTION_NOT_HANDLED
                else:
                    print("=== 二次异常（没人接，就是要命的那一个）")
                    self.report_exception(info)
                    self.report_context()
                    cont = DBG_EXCEPTION_NOT_HANDLED
            elif code == EXIT_PROCESS_DEBUG_EVENT:
                exit_code = u32(info, 0)
                print("进程退出，退出码 %#x (%d)" % (exit_code & 0xFFFFFFFF,
                                                    exit_code & 0x7FFFFFFF
                                                    if exit_code & 0x80000000
                                                    else exit_code))
                kernel32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont)
                return
            kernel32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont)
        print("超时 %d 秒，进程还在跑（说明没崩）" % timeout)

    def dll_name(self, base, info):
        # LOAD_DLL_DEBUG_INFO：hFile@0、lpBaseOfDll@8、…、lpImageName@24、fUnicode@32。
        # lpImageName 是指向"由本进程持有"的字符串指针（常常是 NULL），能读就读。
        unicode_name = u32(info, 32) != 0
        ptr = u64(info, 24)
        name = "dll@%#x" % base
        if ptr:
            slot = self.read(ptr, 8)
            if slot and len(slot) >= (4 if self.is32 else 8):
                sp = (struct.unpack_from("<I", slot, 0)[0] if self.is32
                      else struct.unpack_from("<Q", slot, 0)[0])
                raw = self.read(sp, 512)
                if raw:
                    try:
                        if unicode_name:
                            name = raw.decode("utf-16le", "ignore")
                        else:
                            name = raw.decode("mbcs", "ignore")
                        name = name.split("\x00")[0]
                    except Exception:  # noqa: BLE001
                        pass
        return name or ("dll@%#x" % base)


def subprocess_cmdline(exe, args):
    parts = ['"%s"' % exe]
    for a in args:
        parts.append('"%s"' % a if " " in a else a)
    return " ".join(parts)


def main(argv):
    cwd = None
    timeout = 40
    i = 0
    while i < len(argv):
        if argv[i] == "--cwd":
            i += 1
            cwd = argv[i]
        elif argv[i] == "--timeout":
            i += 1
            timeout = int(argv[i])
        else:
            break
        i += 1
    if i >= len(argv):
        raise SystemExit(__doc__)
    exe = argv[i]
    dbg = Dbg(exe, argv[i + 1:], cwd or os.path.dirname(exe))
    print("pid=%d  %s" % (dbg.pid, exe))
    dbg.loop(timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
