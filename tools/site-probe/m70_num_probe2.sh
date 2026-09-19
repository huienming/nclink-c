#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 数值类应答（第二批）：行号 / 进给速度 / 相对坐标 / 开机时间。
#
# 结论（03 册 §4.1.3）：应答 = 请求的前 24 字节（改 msgtype=1、size、[20..23]=0）
# + 每项固定的尾巴（尾巴最后 4 或 8 字节 = 取值）。所以用 `EREP` 回声请求，
# 只把 [24..] 的尾巴和取值补上，再 cut 到应答长度——能过就说明模板对。
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


def u32(value):
    return struct.pack("<I", value).hex()


def f64(value):
    return struct.pack("<d", value).hex()


# 每项：应答总长、[24..] 的尾巴（不含最后那个取值槽）
POSITION_TAIL = ("0000000006000000 10000000 05000300 00000000".replace(" ", ""))
TAILS = {
    "GetLineNumber": (40, 0x1C, "0000000003000000 04000000".replace(" ", "")),
    "GetFeedSpeed": (52, 0x28, "0000000006000000 10000000 07000200"
                               " 00000000".replace(" ", "")),
    "GetRelativePositionX": (52, 0x28, POSITION_TAIL),
    "GetRelativePositionY": (52, 0x28, POSITION_TAIL),
    "GetRelativePositionZ": (52, 0x28, POSITION_TAIL),
}


def spec_for(item, value, tail_override=None):
    total, size, tail = TAILS[item]
    if tail_override is not None:
        tail = tail_override
    at = 24 + len(tail) // 2
    raw = f64(value) if total - at == 8 else u32(int(value))
    return ("EREP:7:01,8:%s,20:00000000,24:%s,%d:%s,cut:%d"
            % (u32(size), tail, at, raw, total))


print("=== 行号（uint32）")
for value in (7, 1234, 0x0A):
    print("  %-10d %s" % (value, brief(once("GetLineNumber",
                                            spec_for("GetLineNumber", value)))))

print("=== 进给速度（double）")
for value in (0.0, 123.5, -1.25):
    print("  %-10s %s" % (value, brief(once("GetFeedSpeed",
                                            spec_for("GetFeedSpeed", value)))))

print("=== 相对坐标（double）")
for item in ("GetRelativePositionX", "GetRelativePositionY",
             "GetRelativePositionZ"):
    print("  %-22s %s" % (item, brief(once(item, spec_for(item, 12.5)))))

print("=== 尾巴里那个字（[36..39]）扫一下")
for word in ("07000200", "05000300", "07000300", "05000200", "00000000"):
    tail = "000000000600000010000000%s00000000" % word
    print("  %-10s FeedSpeed %s" % (word,
                                    brief(once("GetFeedSpeed",
                                               spec_for("GetFeedSpeed", 1.5,
                                                        tail)))))
    print("  %-10s PosX      %s" % (word,
                                    brief(once("GetRelativePositionX",
                                               spec_for("GetRelativePositionX",
                                                        1.5, tail)))))
PY

echo "=== 各项的请求（各连接第一帧）"
grep -A7 -- "--- request" "$work/mock.log" | grep -E 'request|^[0-9a-f]{4} ' || true
