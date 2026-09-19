#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# The gateway serves its own OpenAPI document at /api.json — that is the full
# route table (every module and every item path). Dump it and print the paths.
#
#   /hp2x    read-only mount of .../app1/hp2x
#   /work    this directory
set -e

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - <<'PY'
import collections, json, urllib.request

spec = json.loads(urllib.request.urlopen("http://127.0.0.1:33123/api.json",
                                         timeout=20).read())
paths = sorted(spec.get("paths", {}))
print("== %d paths" % len(paths))
groups = collections.OrderedDict()
for p in paths:
    parts = p.strip("/").split("/")
    key = "/".join(parts[:2]) if len(parts) > 1 else p
    groups.setdefault(key, []).append("/" + "/".join(parts[2:]) if len(parts) > 2
                                      else "/")
for key, items in groups.items():
    print("--- %s (%d)" % (key, len(items)))
    print("   ", " ".join(items[:40]))
open("/tmp/run/routes.txt", "w").write("\n".join(paths))
PY

kill $gw_pid 2>/dev/null || true
wait 2>/dev/null || true
