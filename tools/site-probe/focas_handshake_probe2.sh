#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# FOCAS 握手第二轮：按反汇编出来的"应答块"结构造应答，扫一遍看 rc。
#
# 反汇编结论（见 protocal/docs/01-FANUC-CNC-FOCAS.md §2.3）：
#   Pdu 对象 = [0..10) 线上的 10 字节头 + [10..] 体；体长在 [8..10)（BE16）
#   Pdu::receive 额外两条长度公式：
#     [6]==0x01 时：体长必须是 16 + 8n，n = be16(体[8..10))（= 记录数）
#     [6]==0x21 时：体长 > 1 且 be16(体[0..2)) != 0（= 应答块个数）
#   Pdu::getRbPos(i)：块 0 在 体[2..]，每块**头 2 字节 = 本块字节数**（BE16），
#                     逐块往后走；体[0..2) = 块个数（i 必须 < 它）
#   Pdu::getRb(i)   ：块 [8..10) = 返回码（BE16），非 0 抛 ErrObj
#   SockPair::system_info_v1：读块 [16..34)（18 字节），所以块 ≥ 34 字节
#   SockPair::request：func 取 0x21 时连发；func=2 要轮询到应答 [6]==2
#
#   /svc    read-only mount of .../app1/FANUC/FOCAS2   (libfwlib32.so)
set -e

cd /work
run=/tmp/run
mkdir -p "$run"
# OUT=相对路径 时把输出写文件
[ -n "$OUT" ] && exec >"$OUT" 2>&1
gcc -O0 -g -o probe focas_probe.c -ldl
: >/work/fwlibeth.log

python3 /work/mock.py 8193 "@$run/reply.txt" >"$run/mock.log" 2>&1 &
sleep 1
export LD_LIBRARY_PATH=/svc

# 应答帧：a0a0a0a0 | 0001 | 功能码 | 方向 02 | 体长 | 体
resp() { printf 'a0a0a0a00001%s%s%04x%s' "$1" "$2" "$((${#3} / 2))" "$3"; }

# 34 字节应答块：块长 0x22，返回码 [8..10) = 0，载荷 [16..34) = 0
BLK34=0022
i=0; while [ $i -lt 7 ];  do BLK34="$BLK34"0000; i=$((i + 1)); done   # [2..16)
i=0; while [ $i -lt 18 ]; do BLK34="$BLK34"00;   i=$((i + 1)); done   # [16..34)

# 一应答 = 块个数 1 + 一个块
BODY1="0001$BLK34"

# func 01 的应答体（16 字节 = 8 个 u16）：
#   [0..2)  → this+116   [2..4) → this+118（2/3 会走特殊分支）
#   [8..10) → 记录数 n，后面跟 n 个 8 字节记录
f1() { printf '0000%04x00000000%04x000000000000' "$1" "$2"; }
# $1 = this+118 的取值（0 / 2 / 3），$2 = 记录数 n；[10..16) 填非零值便于观测

try() {
    printf 'MAP:6:1:01=%s|21=%s|02=%s|%s' "$1" "$2" "$3" "$1" >"$run/reply.txt"
    sleep 0.3
    before=$(wc -l <"$run/mock.log")
    echo "== $4"
    { ./probe 127.0.0.1 8193 || echo "   probe exited $?"; } | sed -n '1,4p'
    echo "   --- 客户端这次发了:"
    tail -n +$((before + 1)) "$run/mock.log" | grep -E 'request|connect from' |
        sed 's/^/   /'
}

echo "########## 第一轮：func 01 走哪条分支 × func 21/02 回块"
try "$(resp 01 02 "$(f1 0 0)")" "$(resp 21 02 "$BODY1")" "$(resp 02 02 "$BODY1")" \
    "f1[2..4)=0  -> 走 else 分支（多发一条 0x21）"
try "$(resp 01 02 "$(f1 3 0)")" "$(resp 21 02 "$BODY1")" "$(resp 02 02 "$BODY1")" \
    "f1[2..4)=3  -> this+118==3，不发第二次 0x21"
try "$(resp 01 02 "$(f1 2 0)")" "$(resp 21 02 "$BODY1")" "$(resp 02 02 "$BODY1")" \
    "f1[2..4)=2  -> 走 0x8d 分支"
try "$(resp 01 02 "$(f1 0 0)")" "$(resp 21 02 "$BODY1")" "$(resp 02 02 "")" \
    "同上，但 func 02 回空体"

echo
echo "########## 第二轮：func 01 体长/记录数"
try "$(resp 01 02 "$(f1 0 0)")" "$(resp 21 02 "$BODY1")" "$(resp 02 02 "$BODY1")" \
    "基准（重复一次，看稳定性）"
# n = 1 的记录，记录内容全 0 → 不产生 Cb、不调 getRb
try "$(resp 01 02 "$(f1 0 1)0000000000000000")" \
    "$(resp 21 02 "$BODY1")" "$(resp 02 02 "$BODY1")" "记录数 1（记录全 0）"

echo
echo "########## 第三轮：func 21 的应答块换个形状"
for blk in "0022" "0024" "0028" "0020"; do
    body="0001$blk"
    i=0; while [ $i -lt 7 ]; do body="$body"0000; i=$((i + 1)); done
    n=$((0x${blk#00} - 16))
    i=0; while [ $i -lt $n ]; do body="$body"00; i=$((i + 1)); done
    try "$(resp 01 02 "$(f1 3 0)")" "$(resp 21 02 "$body")" "$(resp 02 02 "$body")" \
        "块长 0x${blk#00}"
done

echo
echo "===== 最后一次的完整 mock.log 摘要 ====="
tail -40 "$run/mock.log"
