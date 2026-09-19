#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 文件层第三次：找"目录读完"的标志位。
#   mochaFSReadDirectory 的应答：状态 [20..23]（0 = 正常），文件名 = [40..]
#   里第一个 \0 之前的内容；长度必须 ≥ 40。状态 0 时会一直轮询，所以试
#   [24..27]/[28..31]/[32..35]/[36..39] 这几个格子哪个是"还有下一条"。
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

timeout 180 python3 - <<'PY' || echo "（python 这段超时退出了）"
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


def once(item, spec, extra=None):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    body = {"connectionId": conn}
    body.update(extra or {})
    return post(ROOT + "/" + item, body)


def frame(name, block=None):
    """40 + len(name) 字节的读目录应答；block 里给 [24..39] 的格子。"""
    total = 40 + len(name)
    parts = ["ZREP:%d" % total, "7:01", "8:%s" % (total - 12).to_bytes(
        4, "little").hex(), "20:00000000"]
    for offset, value in sorted((block or {}).items()):
        parts.append("%d:%s" % (offset, struct.pack("<I", value).hex()))
    if name:
        parts.append("40:%s" % name.encode().hex())
    return ",".join(parts)


OK = frame("")
print("=== 目录读完 = 哪个格子非 0？")
for offset in (0, 24, 28, 32, 36):
    # [24..39] 对应的栈/应答偏移就是 24+格子
    at = 24 + offset
    node = frame("O1000", {at: 1})
    spec = "SEQ:%s|%s|%s" % (OK, OK, node)
    print("  [%d..%d]=1  %s" % (at, at + 3, brief(once("GetFileList", spec))))

print("=== 读文件：文件名放 [40..]，长度放 [36..39]（照 GetProgramName 的规矩）")
print("  ReadFile %s" % brief(once(
    "ReadFile", "ZREP:45,7:01,8:21000000,20:00000000,36:05000000,"
                "40:48454c4c4f", {"fileName": "O1000"})))
print("=== 读文件：长度放 [32..35]")
print("  ReadFile %s" % brief(once(
    "ReadFile", "ZREP:45,7:01,8:21000000,20:00000000,32:05000000,"
                "40:48454c4c4f", {"fileName": "O1000"})))
PY

echo "=== 请求（前 24 行）"
grep -A6 -- "--- request" "$work/mock.log" | head -32 || true
