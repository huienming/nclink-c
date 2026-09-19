#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 PLC 收尾这一轮：
#   ① 写应答按 SetResponse 的形状（11 字节头）重来；
#   ② 软元件码表核对（deviceType → 请求字节里的设备码）；
#   ③ MC/SLMP 的 Read/Write 到底哪条能用；
#   ④ SLMP 为什么永远 unexpected EOF（反汇编给的解释：给 binary.Read 喂了
#      1 字节的 bytes.Reader 却要读 uint16）——用"超长应答"反证。
#
#   /hp2x  read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 6000,5534 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
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
        return out[-110:]
    data = parsed.get("data") or {}
    if not data.get("success", True):
        return "x %s" % str(data.get("error"))[-90:]
    return repr(data.get("value") if "value" in data else data)


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


def open_conn(root, port):
    opened = post(root + "/Open/TCP", {"ipAddress": "127.0.0.1",
                                       "port": port, "timeout": 3})
    try:
        return (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        return None


READ_ACK = "d00000ffff0300" + "0a00" + "0000" + "1100220033004400"
WRITE_ACK = "d00000ffff0300" + "0200" + "0000"

print("=== ① MC/Write 用 11 字节头（补上站号字节）")
conn = open_conn("/Mitsubishi/Plc/MC", 6000)
set_reply(WRITE_ACK)
print("  %s" % brief(post("/Mitsubishi/Plc/MC/Write",
                         {"connectionId": conn, "deviceType": "D", "index": 100,
                          "values": [1, 2, 3, 4]})))
print("  设备侧请求 %s" % requests()[-1])

print("=== ② 软元件码表核对（读 1 个字，看请求里的设备码）")
set_reply("d00000ffff0300" + "0400" + "0000" + "1100")
for device in ("D", "M", "X", "Y", "L", "F", "V", "B", "SM", "SD", "R", "W",
               "ZR", "TN", "CN", "TS", "CS", "Z", "T", "C", "S", "E"):
    conn = open_conn("/Mitsubishi/Plc/MC", 6000)
    out = post("/Mitsubishi/Plc/MC/Read", {"connectionId": conn,
                                          "deviceType": device, "index": 100,
                                          "size": 1})
    raw = requests()[-1]
    print("  %-3s %-40s %s" % (device, brief(out), raw[24:]))

print("=== ③ 位软元件的地址编码（M100 / X0 / Y177）")
for device, index, size in (("M", 100, 16), ("X", 0, 8), ("Y", 0x1f, 8),
                            ("D", 100, 4), ("D", 65535, 1)):
    conn = open_conn("/Mitsubishi/Plc/MC", 6000)
    set_reply("d00000ffff0300" + "0400" + "0000" + "1100")
    out = post("/Mitsubishi/Plc/MC/Read", {"connectionId": conn,
                                          "deviceType": device, "index": index,
                                          "size": size})
    print("  %s%-6d %-34s %s" % (device, index, brief(out), requests()[-1][24:]))

print("=== ④ SLMP：超长应答也照样 unexpected EOF（1 字节 reader 读 uint16）")
long_ack = "d00000ffff0300" + "%02x00" % (2 + 60) + "0000" + "11" * 60
for label, reply in (("短(19B)", READ_ACK), ("长(71B)", long_ack),
                     ("4E d4 长(71B)",
                      "d400000000" + "00ffff03" + "00" + "%02x00" % (2 + 60)
                      + "0000" + "11" * 60)):
    conn = open_conn("/Mitsubishi/Plc/SLMP", 5534)
    set_reply(reply)
    print("  Read  %-14s %s" % (label, brief(post("/Mitsubishi/Plc/SLMP/Read",
                                                 {"connectionId": conn,
                                                  "deviceType": "D",
                                                  "index": 100, "size": 4}))))
for label, reply in (("短(11B)", WRITE_ACK), ("长(71B)", long_ack)):
    conn = open_conn("/Mitsubishi/Plc/SLMP", 5534)
    set_reply(reply)
    print("  Write %-14s %s" % (label, brief(post("/Mitsubishi/Plc/SLMP/Write",
                                                 {"connectionId": conn,
                                                  "deviceType": "D",
                                                  "index": 100,
                                                  "values": [1, 2, 3, 4]}))))

print("=== ⑤ SLMP IsConnected")
print("  %s" % brief(post("/Mitsubishi/Plc/SLMP/IsConnected", {})))
conn = open_conn("/Mitsubishi/Plc/SLMP", 5534)
print("  建连后 %s" % brief(post("/Mitsubishi/Plc/SLMP/IsConnected",
                                {"connectionId": conn})))

print("=== ⑥ MC 读的容量边界（size 变化）")
def ack(data_hex):
    """标准 3E 应答：11 字节头（长度字段 = 数据长 + 2）+ 数据。"""
    count = len(data_hex) // 2 + 2
    return ("d00000ffff0300" + "%02x%02x" % (count & 0xFF, count >> 8)
            + "0000" + data_hex)


for size in (1, 4, 480, 960, 961):
    set_reply(ack("11" * (2 * size)))
    conn = open_conn("/Mitsubishi/Plc/MC", 6000)
    out = post("/Mitsubishi/Plc/MC/Read", {"connectionId": conn,
                                          "deviceType": "D", "index": 0,
                                          "size": size})
    head = brief(out)
    print("  size=%-4d %s  请求长度字段=%s" % (size, head[:60],
                                            requests()[-1][14:18]))
PY

echo "=== 网关日志尾部"
tail -3 "$work/hp2x.log" || true
