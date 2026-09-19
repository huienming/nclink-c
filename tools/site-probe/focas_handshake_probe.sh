#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# FOCAS2 握手：把反汇编推出来的帧格式喂回去，看 cnc_allclibhndl3 认不认。
#
# 反汇编 libfwlib32.so 得（Pdu::receive 0x23a94）：
#   应答 = 10 字节头 + 体
#     [0..4)  a0 a0 a0 a0（魔数 = 字面量 0x24ac0；#23fbc 做 32 位字节交换后比）
#     [4..6)  be16 类型：<=2 -> 体上限 0x5aa(1450)；=3 -> 0xb5e(2910)；>3 -> 0xd8e(3470)
#     [6]     功能码，必须 == 调用方期望的那个字节（#24484）
#     [7]     1..4（#25230 判范围）
#     [8..10) be16 = 体长度（字节），必须 <= 上面那个上限（#25624）
#     [10..)  体（按体长读满）
# 请求帧同构：12 字节那条 = 头 10 字节 + 体 `00 01`（体长字段 0x0002 ✓）；
# 10 字节那条 = 无体（体长字段 0x0000 ✓）。
#
#   /svc    read-only mount of .../app1/nclink-service
set -e

cd /work
run=/tmp/run
mkdir -p "$run"
gcc -O0 -g -o probe focas_probe.c -ldl
: >/work/fwlibeth.log

EMPTY="a0a0a0a0000121020000"
EMPTY2="a0a0a0a0000102020000"

python3 /work/mock.py 8193 "@$run/reply.txt" >"$run/mock.log" 2>&1 &
sleep 1

export LD_LIBRARY_PATH=/svc

try() {
    printf 'MAP:6:1:01=%s|21=%s|02=%s|%s' "$2" "$3" "$4" "$2" >"$run/reply.txt"
    sleep 0.2
    echo "== 试法 $1"
    ./probe 127.0.0.1 8193 || echo "== probe exited $?"
}

# 系统信息体 16 字节：this+116=be16[0..2) this+118=be16[2..4) …（第 3 格 == 2
# 会让驱动去发那条 func 21 的 40 字节帧）
sysinfo() {
    printf 'a0a0a0a0000101020010%s' "$1"
}

try "系统信息 [2..4)=0（不发 21）" "$(sysinfo 00010000000000000000000000000000)" "$EMPTY" "$EMPTY2"
# 判据（Pdu::receive 0x25ecc）：应答 [7]==2、[6]==1，且体长 == n*8+16
# （n = 应答体 [8..10) 的大端值，type<=2 时）——所以"一律回同一个 16 字节体"才对
try "一律回 16 字节体 [2..4)=2" "$(sysinfo 00010002000000000000000000000000)" \
    "$(sysinfo 00010002000000000000000000000000)" \
    "$(sysinfo 00010002000000000000000000000000)"
try "系统信息 [2..4)=2 + 21 回空体" "$(sysinfo 00010002000000000000000000000000)" "$EMPTY" "$EMPTY2"

# 只回 dir=1 的那两条 12 字节帧（func 21 / 02 那两条不回）
printf 'MAP:6:1:01=%s' "$(sysinfo 00010002000000000000000000000000)" >"$run/reply.txt"
sleep 0.2
echo "== 试法 只回 func 01"
./probe 127.0.0.1 8193 || echo "== probe exited $?"

echo
echo "===== fwlibeth.log（库自己的日志，报错原因在这）====="
tail -20 /work/fwlibeth.log || true
echo
echo "===== 客户端发了什么 ====="
grep -A3 -- "--- request" "$run/mock.log" | head -40 || true
