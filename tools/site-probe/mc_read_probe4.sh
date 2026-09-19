#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 PLC 第四轮：位软元件（M/X/Y）的应答怎么摆、点数上限与分片、
# 以及长度字段给错时的表现。位软元件在 3E 二进制里是"1 字节 2 点"打包的
# （每半字节一个点位），Word 是 1 字 2 字节。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 6000 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
sleep 4

python3 - <<'PY'
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=15).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                      # noqa: BLE001
        return "ERR %s" % exc


def brief(out, limit=8):
    try:
        parsed = json.loads(out)
    except Exception:                             # noqa: BLE001
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-80:]
    value = data.get("value") if "value" in data else data
    if isinstance(value, list):
        return "%d 项 %s" % (len(value), value[:limit])
    return repr(value)


def requests():
    log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    out = []
    for block in log.split("--- request")[1:]:
        lines = block.split("\n")
        out.append("".join(line[6:53].replace(" ", "") for line in lines[1:5]
                           if line.strip()))
    return out


def set_reply(text):
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(text)


def open_conn(port=6000):
    opened = post("/Mitsubishi/Plc/MC/Open/TCP", {"ipAddress": "127.0.0.1",
                                                 "port": port, "timeout": 3})
    try:
        return (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        return None


def ack(data_hex, endcode="0000"):
    count = len(data_hex) // 2 + 2
    return ("d00000ffff0300" + "%02x%02x" % (count & 0xFF, count >> 8)
            + endcode + data_hex)


print("=== ① 位软元件 M100 起 8 点：应答里 1 字节 = 2 点（半字节）")
# 8 点 -> 4 字节；把第 0/2/4/6 位置 1：半字节对 = (1,0)(1,0)(1,0)(1,0) -> 01 01 01 01
for label, data in (("01 01 01 01", "01010101"),
                    ("10 10 10 10", "10101010"),
                    ("00 01 00 01", "00010001"),
                    ("0f 0f 0f 0f", "0f0f0f0f")):
    set_reply(ack(data))
    conn = open_conn()
    out = post("/Mitsubishi/Plc/MC/Read", {"connectionId": conn,
                                          "deviceType": "M", "index": 100,
                                          "size": 8})
    print("  应答 %-12s %s" % (label, brief(out, 8)))
    print("       设备侧 %s" % requests()[-1])

print("=== ② 位软元件 M100 起 16 点（同一批 8 字节应答再发一次）")
set_reply(ack("0101010101010101"))
conn = open_conn()
print("  %s" % brief(post("/Mitsubishi/Plc/MC/Read",
                         {"connectionId": conn, "deviceType": "M", "index": 100,
                          "size": 16}), 20))

print("=== ③ 位软元件的写：M100 起 4 点 values=[1,0,1,1]")
set_reply("d00000ffff0300" + "0200" + "0000")
conn = open_conn()
print("  %s" % brief(post("/Mitsubishi/Plc/MC/Write",
                         {"connectionId": conn, "deviceType": "M", "index": 100,
                          "values": [1, 0, 1, 1]})))
print("  设备侧 %s" % requests()[-1])

print("=== ④ 字软元件的点数上限（请求条数 / 返回值个数）")
for size in (1, 4, 480, 960, 961, 2000):
    before = len(requests())
    set_reply(ack("11" * (2 * size)))
    conn = open_conn()
    out = post("/Mitsubishi/Plc/MC/Read", {"connectionId": conn,
                                          "deviceType": "D", "index": 0,
                                          "size": size})
    after = requests()
    print("  size=%-5d 请求 %d 条  长度字段 %s  请求尾 %s  %s"
          % (size, len(after) - before, after[-1][14:18], after[-1][-8:],
             brief(out, 3)))

print("=== ⑤ 长度字段与真数据不符")
for label, reply in (("长度少 2（数据多 2 字节）", ack("1100")[:-4]),
                     ("长度多 2", "d00000ffff0300" + "0600" + "0000" + "1100"),
                     ("结束码非 0", ack("1100", "0500"))):
    set_reply(reply)
    conn = open_conn()
    print("  %-24s %s" % (label, brief(post("/Mitsubishi/Plc/MC/Read",
                                            {"connectionId": conn,
                                             "deviceType": "D", "index": 0,
                                             "size": 1}))))
PY

echo "=== 网关日志尾部"
tail -3 "$work/hp2x.log" || true
