#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 三菱 PLC：MC / SLMP 两条通道 —— 06 册的格式来自公开资料，这一轮跟网关实测对照。
#   路由：/Mitsubishi/Plc/MC/{Open/TCP,Read,Write}、/SLMP/{Open/TCP,Read,Write,IsConnected}
#   Read 体：{connectionId, deviceType:"D", index:100, size:4}
#   Write 体：{connectionId, deviceType:"D", index:100, values:[1,2,3,4]}
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
import re
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
        error = str(data.get("error"))
        match = re.search(r"(invalid[^\\\"]*|short read[^\\\"]*|"
                          r"unexpected[^\\\"]*|EOF|i/o timeout|"
                          r"exception recovered: [^\\\"]*)", error)
        return "✗ %s" % (match.group(0) if match else error[-90:])
    return repr(data.get("value") if "value" in data else data)


def last_request():
    log = open("/tmp/run/mock.log", encoding="utf-8", errors="replace").read()
    block = log.split("--- request")[-1].split("\n")
    return "".join(line[6:53].replace(" ", "") for line in block[1:4]
                   if line.strip())


def framed(root, label, read_body, write_body, port=6000, reply=""):
    opened = post(root + "/Open/TCP", {"ipAddress": "127.0.0.1",
                                       "port": port, "timeout": 3})
    conn = None
    try:
        conn = (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        pass
    print("=== %s：Open connectionId=%s" % (label, conn))

    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(reply)
    body = {"connectionId": conn}
    body.update(read_body)
    print("  Read     %s" % brief(post(root + "/Read", body)))
    print("           设备侧 %s" % last_request())

    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(reply)
    body = {"connectionId": conn}
    body.update(write_body)
    print("  Write    %s" % brief(post(root + "/Write", body)))
    print("           设备侧 %s" % last_request())


framed("/Mitsubishi/Plc/MC", "MC",
       {"deviceType": "D", "index": 100, "size": 4},
       {"deviceType": "D", "index": 100, "values": [1, 2, 3, 4]})
framed("/Mitsubishi/Plc/SLMP", "SLMP",
       {"deviceType": "D", "index": 100, "size": 4},
       {"deviceType": "D", "index": 100, "values": [1, 2, 3, 4]}, port=5534)

print("=== SLMP IsConnected")
print("  %s" % brief(post("/Mitsubishi/Plc/SLMP/IsConnected", {})))

# 标准 3E 应答：副头 D0 00 + 00 FF FF 03 + 长度(LE) + 结束码 00 00 + 数据
READ_ACK = "d00000ffff03" + "0a00" + "0000" + "1100220033004400"
WRITE_ACK = "d00000ffff03" + "0200" + "0000"
print("=== 用标准 3E 应答回一次")
framed("/Mitsubishi/Plc/MC", "MC(应答)",
       {"deviceType": "D", "index": 100, "size": 4},
       {"deviceType": "D", "index": 100, "values": [1, 2, 3, 4]},
       reply=READ_ACK)
framed("/Mitsubishi/Plc/SLMP", "SLMP(应答)",
       {"deviceType": "D", "index": 100, "size": 4},
       {"deviceType": "D", "index": 100, "values": [1, 2, 3, 4]},
       port=5534, reply=READ_ACK)
print("=== 写应答（只结束码）")
framed("/Mitsubishi/Plc/MC", "MC(写应答)",
       {"deviceType": "D", "index": 100, "size": 4},
       {"deviceType": "D", "index": 100, "values": [1, 2, 3, 4]},
       reply=WRITE_ACK)

print("=== MC Read 数据偏移试三种（值应该是 17/34/51/68）")
VARIANTS = {
    "长度=8、不要结束码": "d00000ffff03" + "0800" + "1100220033004400",
    "长度=10、数据前多 2 字节": "d00000ffff03" + "0a00" + "0000" + "0000"
                                + "1100220033004400",
    "大端字序": "d00000ffff03" + "0a00" + "0000" + "0011002200330044",
    "长度=10、数据 8 字节 + 尾巴": "d00000ffff03" + "1000" + "0000"
                                 + "1100220033004400",
}
for label, spec in VARIANTS.items():
    print("  %-22s %s" % (label, brief(post("/Mitsubishi/Plc/MC/Read",
                                            {"connectionId": None}))
                          if False else ""))
for label, spec in VARIANTS.items():
    opened = post("/Mitsubishi/Plc/MC/Open/TCP", {"ipAddress": "127.0.0.1",
                                                  "port": 6000, "timeout": 3})
    conn = (json.loads(opened).get("data") or {}).get("connectionId")
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    out = post("/Mitsubishi/Plc/MC/Read", {"connectionId": conn,
                                           "deviceType": "D", "index": 100,
                                           "size": 4})
    print("  %-22s %s" % (label, brief(out)))
PY

echo "=== 网关日志尾部"
tail -5 "$work/hp2x.log" || true
