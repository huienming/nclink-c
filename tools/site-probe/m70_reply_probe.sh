#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 M70：应答侧那"唯一还缺的一格"。
# 05 册 §3.2/§3.3 从 C# 交付与 Protol 抓包里抄出了应答布局，这里用假机床
# **逐条验证**它，看网关到底从哪个偏移取值、取什么类型。
#
# 手法：`EREP:` 把网关自己的请求**回声**出来，只改必要的字节（消息类型/IDL 标记/
# 数据区），这样请求 ID、长度、GUID 都不用我们猜。
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
    """每试一次都新开连接——失败会被缓存在连接上。"""
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/" + item, {"connectionId": conn})


# 05 册 §3.2：应答 = 请求头改消息类型(7)=01、请求 ID 原样、28..30 = "IDL" 表示无数据、
# 数据从 36 起。先按"回声 + 打补丁"逐条试：
DOUBLE_42 = struct.pack("<d", 42.0).hex()
INT32_7 = struct.pack("<i", 7).hex()
CASES = [
    ("回声（只改消息类型）", "EREP:7:01"),
    ("回声 + 28..30=IDL", "EREP:7:01,28:49444c"),
    ("回声 + 36 起 INT32=7", "EREP:7:01,28:49444c,36:%s" % INT32_7),
    ("回声 + 36 起 DOUBLE=42", "EREP:7:01,28:49444c,36:%s" % DOUBLE_42),
]

for name, spec in CASES:
    print("  %-32s %s" % (name, brief(once("GetPartCount", spec))))

# "error response length" 是 hp2x/common 里的传输层错误 → 先扫长度：
print("=== GetPartCount：把回声截断到 N 字节（消息类型=Reply，值 INT32=7 在 36）")
for cut in range(36, 85, 4):
    spec = "EREP:7:01,36:%s,cut:%d" % (INT32_7, cut)
    print("  cut=%-3d %s" % (cut, brief(once("GetPartCount", spec))))

print("=== GetPartCount：改 offset 8 的 GIOP 长度字段")
for size, cut in [(0x28, 40), (0x2c, 56), (0x44, 80)]:
    spec = "EREP:7:01,8:%s,36:%s,cut:%d" % (
        struct.pack("<I", size).hex(), INT32_7, cut)
    print("  size=0x%-3x cut=%-3d %s" % (size, cut,
                                         brief(once("GetPartCount", spec))))

print("=== GetStatus：逐字节找哪个 byte 决定状态名（36..48 每个偏移各喂 0..4）")
for offset in range(36, 49):
    row = []
    for value in (0, 1, 2, 3, 4):
        row.append(brief(once("GetStatus", "EREP:7:01,%d:%02x" % (offset, value))))
    print("  offset %-3d %s" % (offset, "  ".join(row)))
PY

echo "=== mock.log 里第一次 GetPartCount 的请求（80 字节 GIOP）"
grep -m1 -A6 "request" "$work/mock.log" || true
