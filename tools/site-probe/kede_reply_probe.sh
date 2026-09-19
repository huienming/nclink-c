#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# KEDE's reply format, discovered by letting the gateway's own parser talk: every
# rejected reply names the element it expected ("expected element type <x> but
# have <y>"). Try a handful of candidate bodies and print what it says.
#
#   /hp2x    read-only mount of .../app1/hp2x
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

printf '' >"$work/reply.txt"
python3 /work/mock.py 62937 "@$work/reply.txt" >"$work/mock.log" 2>&1 &
mock_pid=$!
sleep 1

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - <<'PY'
import json, urllib.error, urllib.request

base = "http://127.0.0.1:33123"
reply_file = "/tmp/run/reply.txt"


def post(path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        return urllib.request.urlopen(req, timeout=10).read().decode("utf-8",
                                                                     "replace")
    except urllib.error.HTTPError as exc:
        return "HTTP %d %s" % (exc.code, exc.read().decode("utf-8", "replace"))
    except Exception as exc:                  # noqa: BLE001
        return "ERR %s" % exc


opened = json.loads(post("/KEDE/CNC/GNC62/Open/TCP",
                         {"ipAddress": "127.0.0.1", "port": 62937, "timeout": 3}))
conn = (opened.get("data") or {}).get("connectionId")
print("connectionId = %s" % conn)


def ask(xml, label):
    """xml may use {uid} / {req} / {reqflip}: the mock fills them from the
    request it just received (the gateway checks the reply's uid)."""
    with open(reply_file, "w") as handle:
        handle.write("XSUB:" + xml + "\n")
    out = post("/KEDE/CNC/GNC62/PART_COUNT", {"connectionId": conn})
    print("--- %-46s %s" % (label, out[:220]))


# The gateway's own xml:"..." struct tags (159 of them, extracted from the Go
# binary) are the element vocabulary — the earlier guesses (val/value/cnt...)
# were simply not in it. Put the real candidates in one reply with distinct
# numbers: whichever comes back is the one that carries the value.
names = ["nmb", "no", "max", "v1", "v2", "v3", "act", "act1", "state1", "bar",
         "bar1", "info", "ret", "status", "mode", "mmode", "amode", "tool",
         "unit", "ax1", "position1", "preset1", "over1", "over", "tm"]
children = "".join("<%s>%d</%s>" % (name, i + 1, name)
                   for i, name in enumerate(names))
ask("<counter><req>{reqflip}</req><uid>{uid}</uid><sub>get</sub>"
    + children + "</counter>", "many value children")

# Text inside the root, and attributes on it.
ask("<counter>99</counter>", "root text")
ask('<counter val="98"><req>{reqflip}</req><uid>{uid}</uid></counter>',
    "root attribute val")
ask('<counter value="97"><req>{reqflip}</req><uid>{uid}</uid></counter>',
    "root attribute value")

# And the same trick on another item that returns a string.
def ask_status(xml, label):
    with open(reply_file, "w") as handle:
        handle.write("XSUB:" + xml + "\n")
    out = post("/KEDE/CNC/GNC62/STATUS", {"connectionId": conn})
    print("--- %-46s %s" % ("STATUS " + label, out[:200]))


ask_status("<ncda><req>{reqflip}</req><uid>{uid}</uid><var>ncstate</var>"
           + children + "</ncda>", "many value children")
ask_status("<ncda><req>{reqflip}</req><uid>{uid}</uid><var>ncstate</var>"
           "<val>123</val></ncda>", "val=123")
ask_status("<ncda><req>yes</req><uid>{uid}</uid><var>ncstate</var></ncda>",
           "req=yes")
ask_status("<ncda><req>no</req><uid>{uid}</uid><var>ncstate</var></ncda>",
           "req=no")

# Attributes on the root, each candidate name with a distinct value.
attrs = " ".join('%s="%d"' % (name, i + 1) for i, name in enumerate(names))
# Envelope as attributes, value as text — the other plausible shape.
ask('<counter req="{reqflip}" uid="{uid}">41</counter>', "attrs + text")
ask('<counter req="{reqflip}" uid="{uid}" nmb="41"></counter>', "attr nmb")
ask('<counter req="{reqflip}" uid="{uid}" v1="41" v2="42"></counter>',
    "attr v1/v2")
ask('<counter><req>{reqflip}</req><uid>{uid}</uid><sub>get</sub>'
    "<v1>41</v1><v2>42</v2><v3>43</v3></counter>", "v1..v3 children")
ask('<counter><req>{reqflip}</req><uid>{uid}</uid><sub>get</sub>'
    "<over>41</over></counter>", "over=41")
for xml in candidates:
    ask(xml, xml[:44])
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
