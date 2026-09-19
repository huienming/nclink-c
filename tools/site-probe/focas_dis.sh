#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 反汇编 libfwlib32.so.1 里的 FOCAS 内部函数（Pdu / SockPair / Handle / cnc_*）。
#
#   sh /work/focas_dis.sh                        # 默认：握手链上那几个函数
#   sh /work/focas_dis.sh calls 0x1b534 0x1bad4  # 只列调用序列
#   sh /work/focas_dis.sh full  0x1b534          # 整段反汇编
#   sh /work/focas_dis.sh sym   '_ZN3Pdu7receiveER6Socketih'
#
#   /fw    read-only mount of .../app1/FANUC/FOCAS2
set -e

LIB=/fw/libfwlib32.so.1
MODE=${1:-handshake}
ASM=/tmp/fwlib.asm

# OUT=相对路径 时把输出写文件（长反汇编在终端里不好看）
[ -n "$OUT" ] && exec >"$OUT" 2>&1

# 一次性反汇编，之后按符号名标签切段（objdump 自带 `<符号>:` 标签）
[ -s "$ASM" ] || objdump -d "$LIB" >"$ASM"

block() { # 打出某个符号的整段
    awk -v want="$1" '
        /^[0-9a-f]+ <.*>:$/ { hit = (index($0, "<" want ">") > 0) }
        hit { print }
    ' "$ASM"
}

calls() {
    block "$1" | grep -E '[[:space:]](bl|blx)[[:space:]]' |
        sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]\+/ /g' |
        awk '{ print $1, $4, $5 }' | c++filt
}

case "$MODE" in
    handshake)
        for f in cnc_allclibhndl4 _ZN8SockPair14system_info_v1ER6SocketiR3Pdu \
                 _ZN8SockPair14system_info_v3ER6SocketiR3Pdu; do
            echo "----- $f"
            calls "$f" | sort -u
        done
        ;;
    calls) shift; for f in "$@"; do echo "----- $f"; calls "$f"; done ;;
    full)  shift; for f in "$@"; do echo "----- $f"; block "$f"; done ;;
    who)   shift; for a in "$@"; do
               echo "----- $a"
               awk -v want="$a" '
                   /^[0-9a-f]+ <.*>:$/ { name = $0 }
                   $1 == want ":" { print name; exit }
               ' "$ASM"
           done ;;
    at)    shift; awk -v want="$1" '
               /^[0-9a-f]+ <.*>:$/ { name = $0 }
               $1 == want ":" { print name }
           ' "$ASM" ;;
    lines) shift; sed -n "$1p" "$ASM" ;;
    range) awk -v a="$2:" -v b="$3:" '
               { line = $1 }
               line == a { on = 1 }
               on { print }
               line == b { exit }
           ' "$ASM" ;;
    hs)    # 握手链全景：connect 里建请求那段 + system_info_v1 收尾 + request 本体
           echo "########## SockPair::connect  [1cd80..1ce70) 建请求"
           awk -v a="1cd80:" -v b="1ce70:" '
               { l = $1 } l == a { on = 1 } on { print } l == b { exit }' "$ASM"
           echo "########## SockPair::system_info_v1  [1b7f4..1bad4)"
           awk -v a="1b7f4:" -v b="1bad4:" '
               { l = $1 } l == a { on = 1 } on { print } l == b { exit }' "$ASM"
           echo "########## SockPair::request  (全部)"
           block '_ZN8SockPair7requestER6SocketiR3Pduh'
           echo "########## Cb::Cb  (全部)"
           block '_ZN2CbC1Etttlllltt'
           ;;
    sym)   shift; exec objdump -T "$LIB" | c++filt | grep -F "$1" ;;
    *)     echo "usage: $0 [handshake|calls|full|sym] ..." >&2; exit 2 ;;
esac
