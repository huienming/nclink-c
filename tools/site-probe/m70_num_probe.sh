#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 M70 数值类（GetPartCount 等）应答：拿反汇编里那个"应答模板"当真机回。
#
# 03 册 §4.1.2 已经确认 GetPartCount 的结构：收 40 字节应答 → 先 memequal 比
# 前 36 字节（跟一个驱动自己拼的模板）→ 过了再 binary.Read 读 [36..39] 的
# uint32。所以这里把模板拼出来直接回，看能不能走出成功路径。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf 'EREP:7:01' >"$work/reply.txt"
python3 /work/mock.py 683 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import struct
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=10).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out):
    try:
        data = json.loads(out)["data"]
    except Exception:                             # noqa: BLE001
        return out[:110]
    if not data.get("success"):
        return "✗ %s" % data.get("error")
    return repr(data.get("value"))


def once(item, spec):
    """每次试一次都新开连接——失败会被缓存在连接上。"""
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


# 反汇编拼出来的 40 字节应答模板：[0..35] 固定、[36..39] = uint32 取值。
TEMPLATE = ("47494f50010001011c000000"
            "000000003c1f000000000000"
            "00000000030000000400000008000000")


def tmpl(value, at=36):
    raw = bytearray.fromhex(TEMPLATE)
    struct.pack_into("<I", raw, at, value)
    return bytes(raw).hex()


print("=== GetPartCount：先看驱动发的请求")
print("  " + brief(once("GetPartCount", "EREP:7:01,cut:40")))

print("=== 直接回模板（[36..39] = uint32）")
for value in (7, 1234, 0x68636F6D):
    print("  value=%-12d %s" % (value, brief(once("GetPartCount", tmpl(value)))))

print("=== 回声 + 只改必要字节（验证模板是不是=请求的变形）")
CASES = [
    ("回声 + msgtype/size", "EREP:7:01,8:1c000000,cut:40"),
    ("再补 [24..35]", "EREP:7:01,8:1c000000,24:00000000,28:03000000,"
                      "32:04000000,cut:40"),
    ("再补 [36..39]=0x01020304", "EREP:7:01,8:1c000000,24:00000000,"
                                 "28:03000000,32:04000000,36:04030201,cut:40"),
    ("再补 [36..39]=7", "EREP:7:01,8:1c000000,24:00000000,28:03000000,"
                        "32:04000000,36:07000000,cut:40"),
]
for name, spec in CASES:
    print("  %-24s %s" % (name, brief(once("GetPartCount", spec))))
PY

echo "=== 驱动发的请求（GetPartCount）"
grep -m1 -A6 -- "--- request" "$work/mock.log" || true
