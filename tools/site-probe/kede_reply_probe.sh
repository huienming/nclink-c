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
# Give the parser *every* field name we extracted from the binary, each with a
# distinct number: whichever number comes back names the field it reads.
field_names = """act act1 act2 act3 act4 act5 act6 actf0 actual amode ax1 ax2 ax3
ax4 ax5 ax6 ax7 ax8 ax9 ax10 ax11 ax12 ax13 ax14 ax15 ax16 ax17 ax18 ax19 ax20
ax21 ax22 ax23 ax24 ax25 ax26 ax27 ax28 ax29 ax30 ax31 ax32 backw bar bar1 bar2
bar3 bar4 bar5 bar6 blksel block bsupr debugmode hrel inauto info localtime m01
manrtcp max mmode mode ncstate nmb no over over1 over2 over3 over4 over5 over6
overf0 pos position1 position2 position3 position4 position5 position6 preset
preset1 preset2 preset3 preset4 preset5 preset6 prio prg proctime0 proctime1
proctimeproc proctimestart remaindertime ret state1 state2 state3 status switch
tm tool unit v1 v2 v3 v4 v5 v6 v7 v8 v9 var""".split()
index = {name: i + 1 for i, name in enumerate(field_names)}
children_all = "".join("<%s>%d</%s>" % (name, index[name], name)
                         for name in field_names)
ask("<counter><req>{reqflip}</req><uid>{uid}</uid><sub>get</sub>"
    + children_all + "</counter>", "all 100+ fields, distinct values")
ask_status("<ncda><req>{reqflip}</req><uid>{uid}</uid><var>ncstate</var>"
           + children_all + "</ncda>", "all fields, distinct values")

print("field -> number: " +
      ", ".join("%s=%d" % (n, index[n]) for n in field_names[:20]) + " …")

# value 9 == "actual" -> the counter lives in <actual>. Confirm, then look for
# the string item's field the same way (a few plausible spellings).
ask("<counter><req>{reqflip}</req><uid>{uid}</uid><sub>get</sub>"
    "<actual>77</actual></counter>", "actual=77 (expect 77)")
ask_status("<ncda><req>{reqflip}</req><uid>{uid}</uid><var>ncstate</var>"
           "<ncstate>free</ncstate></ncda>", "ncstate=free")
ask_status("<ncda><req>{reqflip}</req><uid>{uid}</uid><var>ncstate</var>"
           "<status>free</status></ncda>", "status=free")
ask_status("<ncda><req>{reqflip}</req><uid>{uid}</uid><var>ncstate</var>"
           "<ncstate>2048</ncstate></ncda>", "ncstate=2048")
for xml in candidates:
    ask(xml, xml[:44])
PY

kill $gw_pid $mock_pid 2>/dev/null || true
wait 2>/dev/null || true
