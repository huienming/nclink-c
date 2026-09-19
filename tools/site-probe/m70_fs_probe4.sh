#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 文件层第四次：把 mochaFSReadDirectory 的"读完"判别位钉死。
#
# 反汇编 0x639e68（mochaFSReadDirectory）+ 0x638b3c（GetFileList）得：
#   应答 [20..23]（LE u32，代码里叫 replyStatus）
#     非 0 -> 这一帧是"目录读完"：GetFileList 直接跳出循环，且**这一帧的
#             文件名被丢掉**（函数此时返回空串），并打印
#             "replyStatus of mochaFSReadDirectory: %v\n"
#     等于 0 -> 这一帧带一条文件名 = 应答 [40..]（\0 会被剔掉），循环继续
#   应答长度 < 24 -> panicSliceAcap；[20..23]==0 而长度 < 40 -> panicSliceB
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
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"


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
        return out[:120]
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


def frame(status, name="", total=None):
    """一帧 mochaFSReadDirectory 应答：头 24 字节 + 尾巴 + [40..] 放文件名。"""
    size = total if total else 40 + len(name)
    parts = ["ZREP:%d" % size, "7:01", "8:%s" % (size - 12).to_bytes(
        4, "little").hex(),
        "20:%s" % status.to_bytes(4, "little").hex()]
    if name:
        parts.append("40:%s" % name.encode().hex())
    return ",".join(parts)


OK = frame(0)
NAME = "6d6f6368614653526561644469726563746f7279"   # "mochaFSReadDirectory"


def seq(parts):
    """只有"读目录"那几次请求会按顺序取 parts；其余（Init / StatFile /
    OpenDirectory / CloseDirectory，还有网关自己的后台轮询）一律回 OK。
    最后一条必须是 replyStatus != 0，否则循环不收敛。"""
    return "LOOPQ:%s/%s/%s" % (NAME, OK, "|".join(parts))


print("=== 两帧带名 + 一帧 replyStatus=1")
print("  GetFileList %s" % brief(once(
    "GetFileList", seq([frame(0, "O1000"), frame(0, "O2000"), frame(1)]))))

print("=== 只有一帧 replyStatus=1")
print("  GetFileList %s" % brief(once("GetFileList", seq([frame(1)]))))

print("=== replyStatus=1 的那帧也写了文件名（按反汇编应当被丢掉）")
print("  GetFileList %s" % brief(once(
    "GetFileList", seq([frame(0, "O1000"), frame(1, "O9999")]))))

print("=== 文件名尾部带两个 \\0（应当被剔掉）")
print("  GetFileList %s" % brief(once(
    "GetFileList", seq([frame(0, "O1000\x00\x00"), frame(1)]))))

print("=== 短应答：20 字节（< 24）")
print("  GetFileList %s" % brief(once("GetFileList", seq(["EREP:7:01,cut:20"]))))

print("=== 短应答：30 字节且 replyStatus=0（< 40）")
print("  GetFileList %s" % brief(once(
    "GetFileList", seq([frame(0, total=30)]))))

print("=== 短应答：30 字节但 replyStatus=1")
print("  GetFileList %s" % brief(once(
    "GetFileList", seq([frame(1, total=30)]))))
PY

echo "=== 请求（前 40 行）"
grep -A6 -- "--- request" "$work/mock.log" | head -46 || true
echo "=== 网关日志里的 replyStatus 行"
grep -n "replyStatus\|fileList\|panic" "$work/hp2x.log" | head -20 || true
