#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# FANUC 机器人 RMI 第二次：用 MAP 按"请求 == 模板 B"来分辨第几帧，
# 免得 SEQ 被网关自己的其他请求插队算歪。
#
#   请求 56 字节 == B（0x819b1f）-> 回 C（0x819b8f）
#   其它请求（56 个 0 / Init2 的 00 00 04 … / GetData 的 CLRASG …）-> 回 A（0x819bff）
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf 'EREP:7:01' >"$work/reply.txt"
python3 /work/mock.py 8193 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

timeout 120 python3 - <<'PY' || echo "（python 这段超时退出了）"
import importlib.util
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/FANUC/ROBOT"

# 模板直接按虚地址从二进制里读，免得抄错一个字符
spec = importlib.util.spec_from_file_location("elf_vaddr", "/work/elf_vaddr.py")
elf_vaddr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf_vaddr)
BLOB = open("/hp2x/hp2x_box200", "rb").read()
SECTS = elf_vaddr.load_sections(BLOB)


def template(vaddr):
    off = elf_vaddr.to_offset(SECTS, vaddr)
    return bytes.fromhex(BLOB[off:off + 112].decode())


A = template(0x819bff).hex()      # Init 第 1 帧的期望应答
B = template(0x819b1f).hex()      # Init 第 2 帧发出去的请求
C = template(0x819b8f).hex()      # Init 第 2 帧的期望应答
PAIR = "MAP:0:56:%s=%s|%s" % (B, C, A)
ZERO = "00" * 56


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=12).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8",
                                                           "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out):
    try:
        data = json.loads(out)["data"]
    except Exception:                             # noqa: BLE001
        return out[:150]
    if not data.get("success"):
        return "✗ %s" % data.get("error")
    return repr(data.get("value") or data)


def once(item, spec, extra=None):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 8193,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    body = {"connectionId": conn}
    body.update(extra or {})
    return post(ROOT + "/" + item, body)


print("=== Init（第 1 帧期望 A、第 2 帧期望 C）")
print("  Init      %s" % brief(once("Init", PAIR)))

print("=== Init2（首帧 00 00 04 …，后面同 Init）")
print("  Init2     %s" % brief(once("Init2", PAIR)))

print("=== Init 第 1 帧回 A、第 2 帧回全 0（数据比对应当不过）")
print("  Init      %s" % brief(once("Init", "MAP:0:56:%s=%s|%s" % (B, ZERO,
                                                                  A))))

# 头 56 字节仍是 A（Init 的两次比对只看前 56 字节），[56..] 放载荷
def with_payload(payload):
    return "MAP:0:56:%s=%s|%s" % (B, C, A + payload)


print("=== GetData：应答 = A + 'HELLO'（载荷在 [56..]）")
print("  GetData   %s" % brief(once("GetData", with_payload(b"HELLO".hex()),
                                   {"axis": "0"})))

print("=== GetData：应答 = A + 32 字节递增（看载荷是不是原样回）")
print("  GetData   %s" % brief(once(
    "GetData", with_payload(bytes(range(32)).hex()), {"axis": "0"})))

print("=== PrvReadUWord：应答 = A + 32 字节递增（u16 数组从 [56..] 取）")
print("  PrvReadUWord %s" % brief(once(
    "PrvReadUWord", with_payload(bytes(range(32)).hex()), {"index": "1"})))

print("=== PrvReadBit：selector 70 / index 1 / count 8")
print("  PrvReadBit %s" % brief(once(
    "PrvReadBit", with_payload(bytes(range(32)).hex()),
    {"selector": "70", "index": "1", "count": "8"})))

print("=== ReadRShort：address 1 / count 10")
print("  ReadRShort %s" % brief(once(
    "ReadRShort", with_payload(bytes(range(32)).hex()),
    {"address": "1", "count": "10"})))

print("=== GetData：带 setasgs（照 api.json 的默认值）")
print("  GetData   %s" % brief(once(
    "GetData", with_payload(bytes(range(32)).hex()),
    {"setasgs": ["SETASG 1 1000 ALM[1] 1", "SETASG 1001 50 POS[0] 0.0"]})))

print("=== GetHexResponse：直接发一段 hex（api.json 默认的那帧）")
print("  GetHexResponse %s" % brief(once(
    "GetHexResponse", with_payload(bytes(range(32)).hex()),
    {"hexData": "020006000000000000010000000000000001000000000000000000000000"
                "06c000000000100e00000101040a00000a000000000000000000"})))
PY

echo "=== mock：连接与请求总览"
grep -c -- "--- connect from" "$work/mock.log" || true
grep -o "^0000  [0-9a-f ]*" "$work/mock.log" | head -20 || true
echo "=== 网关日志尾"
tail -12 "$work/hp2x.log" || true
