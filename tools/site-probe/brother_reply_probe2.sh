#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 兄弟 Brother：应答形状（反汇编 brother/cnc.responseIsOk 0x617704 + filterResData 0x617324）
#
#   responseIsOk 的要求：
#     1. 按 `\r\n` 切开后至少 2 段；
#     2. 第 1 段 ≥ 19 字节、且**以 ASCII "00" 结尾**（就是请求第一行那种排法）；
#     3. 整包的**最后一个字节是 `%`**。
#   不满足 1/2 就报 `request error,response: <我们发的>`（之前卡的就是这一步）。
#
#   所以应答 = <请求第一行原样 19 字节> + CRLF + 数据 + CRLF + `%`。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 10000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import re
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Brother/CNC"


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
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-120:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        error = str(data.get("error"))
        match = re.search(r"(request error[^\\\"]*)", error)
        return "✗ %s" % (match.group(1)[:90] if match else error[-90:])
    return repr(data.get("value"))


def once(item, spec, extra=None):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 10000,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    body = {"connectionId": conn}
    body.update(extra or {})
    return post(ROOT + "/" + item, body)


def reply(data):
    """第 1 段 = 请求前 19 字节（原样），然后 CRLF + 数据 + CRLF + %。"""
    raw = data.encode()
    total = 24 + len(raw)
    return ("ZREP:%d,19:0d0a,21:%s,%d:0d0a25"
            % (total, raw.hex(), 21 + len(raw)))


print("=== 数据长度扫描（GetCurProgName）：看它到底取哪几个字节")
for text in ("A", "AB", "ABC", "ABCD", "ABCDE", "ABCDEF"):
    print("  数据 %-7s → %s" % (text, brief(once("GetCurProgName", reply(text)))))

print("=== PWD")
print("  /       → %s" % brief(once("PWD", reply("/"))))
print("  /O1000  → %s" % brief(once("PWD", reply("/O1000"))))

print("=== GetToolList 试几种摆法")
for text in ("T01,1;T02,2", "1,T01;2,T02", "T01=1;T02=2", "T01 1;T02 2"):
    print("  %-16s → %s" % (text, brief(once("GetToolList", reply(text)))))
PY

echo "=== 请求"
grep -A6 -- "--- request" "$work/mock.log" | head -20 || true
