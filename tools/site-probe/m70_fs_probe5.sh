#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# M70 文件层第五次：补两个"只有反汇编说过、还没实测"的结论。
#   ① 文件名取的是应答 [40..] 整段，**里面的 \0 会被剔掉**；
#   ② 收尾那一帧（replyStatus != 0）**哪怕带文件名也被丢掉**——
#      函数此时返回空串，GetFileList 在 append 之前就跳出循环了。
#
# 这里用 mock.py 的 LOOPQ：只有内容里含 "mochaFSReadDirectory" 的请求才按
# 顺序取应答，其余（Init / StatFile / OpenDirectory / CloseDirectory 以及
# 网关自己的后台轮询）一律拿 filler——因为 SEQ 会被后台轮询打断，不可靠。
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

timeout 120 python3 - <<'PY' || echo "（python 这段超时退出了）"
import json
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:33123"
REPLY = "/tmp/run/reply.txt"
ROOT = "/Mitsubishi/CNC/M70"
NAME = "6d6f6368614653526561644469726563746f7279"      # mochaFSReadDirectory


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


def once(spec):
    opened = json.loads(post(ROOT + "/Open/TCP",
                             {"ipAddress": "127.0.0.1", "port": 683,
                              "timeout": 3}))
    conn = (opened.get("data") or {}).get("connectionId")
    post(ROOT + "/Init", {"connectionId": conn})
    with open(REPLY, "w", encoding="utf-8") as handle:
        handle.write(spec)
    return post(ROOT + "/GetFileList", {"connectionId": conn})


def frame(status, name="", total=None):
    size = total if total else 40 + len(name)
    parts = ["ZREP:%d" % size, "7:01", "8:%s" % (size - 12).to_bytes(
        4, "little").hex(),
        "20:%s" % status.to_bytes(4, "little").hex()]
    if name:
        parts.append("40:%s" % name.encode().hex())
    return ",".join(parts)


OK = frame(0)                 # replyStatus=0、不带名字的空条目
END = frame(1)                # replyStatus=1：目录读完


def loopq(filler, parts):
    return "LOOPQ:%s/%s/%s" % (NAME, filler, "|".join(parts))


print("=== ① 文件名 \"O1000\\0\\0\"（[40..] 整段取，\\0 剔掉）")
print("  GetFileList %s" % brief(once(
    loopq(END, [frame(0, "O1000\x00\x00"), END]))))

print("=== ② 收尾帧也带名字 O9999（应当整帧丢掉，列表为空）")
print("  GetFileList %s" % brief(once(loopq(OK, [frame(1, "O9999")]))))
PY

echo "=== 网关日志（replyStatus / fileList）"
grep -n "replyStatus\|size of fileList" "$work/hp2x.log" || true
echo "=== 每个请求用哪一帧（mock 日志里的请求名）"
grep -o "mochaFS[A-Za-z]*\|mochaGetData" "$work/mock.log" | head -24 || true
