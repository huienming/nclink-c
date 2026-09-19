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


def framed(root, label, read_body, write_body, port=6000):
    opened = post(root + "/Open/TCP", {"ipAddress": "127.0.0.1",
                                       "port": port, "timeout": 3})
    conn = None
    try:
        conn = (json.loads(opened).get("data") or {}).get("connectionId")
    except Exception:                             # noqa: BLE001
        pass
    print("=== %s：Open connectionId=%s" % (label, conn))

    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write("")
    body = {"connectionId": conn}
    body.update(read_body)
    print("  Read     %s" % brief(post(root + "/Read", body)))
    print("           设备侧 %s" % last_request())

    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write("")
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
PY

echo "=== 网关日志尾部"
tail -5 "$work/hp2x.log" || true
