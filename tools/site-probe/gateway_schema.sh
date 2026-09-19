#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 从网关自己的 /api.json 里抄某个模块的请求/应答字段（比猜 req 结构快）。
#
#   sh /work/gateway_schema.sh '/FANUC/ROBOT'
#
#   /hp2x    read-only mount of .../app1/hp2x
set -e

prefix=${1:-/}

work=/tmp/run
rm -rf "$work/hp2x"
mkdir -p "$work/log"
cp -r /hp2x "$work/hp2x"
cd "$work/hp2x"

./hp2x_box200 >"$work/hp2x.log" 2>&1 &
gw_pid=$!
sleep 4

python3 - "$prefix" <<'PY'
import json
import sys
import urllib.request

prefix = sys.argv[1]
spec = json.loads(urllib.request.urlopen("http://127.0.0.1:33123/api.json",
                                         timeout=20).read())
for path in sorted(spec.get("paths", {})):
    if not path.startswith(prefix):
        continue
    print("== %s" % path)
    print("   %s" % json.dumps(spec["paths"][path], ensure_ascii=False)[:1400])

schemas = spec.get("components", {}).get("schemas", {})
tag = prefix.strip("/").replace("/", ".").lower()
for name in sorted(schemas):
    if tag in name.lower():
        print("-- %s" % name)
        print("   %s" % json.dumps(schemas[name], ensure_ascii=False)[:800])
PY

kill $gw_pid 2>/dev/null || true
wait 2>/dev/null || true
