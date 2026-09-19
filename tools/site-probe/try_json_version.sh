#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# The plugins' nlohmann map uses std::less<std::string>, ours (3.11.x) uses
# std::less<void>, so their inlined code mis-reads our json. Fetch the single
# header for an older version and build the harness against that instead.
#
#   sh try_json_version.sh 3.10.5
set -e

ver=${1:-3.10.5}
lib=${2:-libgsk-http.so}
port=${3:-6000}
work=/tmp/run

mkdir -p "$work/json"
if [ ! -f "$work/json/nlohmann/json.hpp" ]; then
    echo "== fetching nlohmann $ver"
    mkdir -p "$work/json/nlohmann"
    python3 - "$ver" <<'PY'
import sys, ssl, urllib.request, urllib.error
ver = sys.argv[1]
out = "/tmp/run/json/nlohmann/json.hpp"
# The probe image has no CA bundle; the header is public, fetches only.
ctx = ssl._create_unverified_context()
last = None
for tag in ("v" + ver, ver):
    url = ("https://raw.githubusercontent.com/nlohmann/json/%s/"
           "single_include/nlohmann/json.hpp" % tag)
    try:
        data = urllib.request.urlopen(url, timeout=60, context=ctx).read()
        open(out, "wb").write(data)
        print("   %s: %d bytes" % (tag, len(data)))
        break
    except Exception as exc:              # noqa: BLE001
        last = exc
        print("   %s: %s" % (tag, exc))
else:
    raise SystemExit("could not fetch nlohmann %s (%s)" % (ver, last))
PY
fi

cd /work
g++ -w -O0 -D_GLIBCXX_USE_CXX11_ABI=0 -I"$work/json" -o drive plugin_drive.cpp -ldl

echo "== default object comparator in this header:"
grep -nE 'default_object_comparator_t|using object_t' "$work/json/nlohmann/json.hpp" |
    head -4

python3 mock.py 33123 'HTTP200:{"code":0,"msg":"ok","data":{"connectionId":"probe"}}' \
    >module.log 2>&1 &
mod_pid=$!
python3 mock.py "$port" >mock.log 2>&1 &
pid=$!
sleep 0.8

echo "== nlohmann $ver: driving $lib at 127.0.0.1:$port"
timeout 20 env LD_LIBRARY_PATH=/svc ./drive "/svc/$lib" 127.0.0.1 "$port" \
    "/GSK/CNC/Open/TCP" "" '{"ipAddress":"127.0.0.1","port":'"$port"',"timeout":5}' \
    2>&1 || echo "== drive exited $?"

sleep 0.5
kill $pid $mod_pid 2>/dev/null || true
wait 2>/dev/null || true
echo "=== to the machine ($port) ==="
cat mock.log
echo "=== to the module server (33123) ==="
head -30 module.log
