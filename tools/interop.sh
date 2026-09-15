#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Run the broker interoperability suite (tests/test_broker.c) against real
# brokers started in Docker.
#
#   ./tools/interop.sh              # Mosquitto and EMQX
#   ./tools/interop.sh mosquitto    # just one of them
#   ./tools/interop.sh emqx
#
# The brokers listen on 18830 (Mosquitto) and 18831 (EMQX) on 127.0.0.1, so a
# broker already running on the usual 1883 is left untouched. The container
# images are pulled on first use.
#
# The test binary is located through $NCL_TEST_BROKER_BIN, otherwise the usual
# build outputs are tried (build-linux, CMake build dir, Windows build dir).
#
# Environment overrides:
#   NCL_INTEROP_MOSQUITTO_IMAGE   default eclipse-mosquitto:2
#   NCL_INTEROP_EMQX_IMAGE        default emqx/emqx:5.8.9

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MOSQUITTO_IMAGE=${NCL_INTEROP_MOSQUITTO_IMAGE:-eclipse-mosquitto:2}
EMQX_IMAGE=${NCL_INTEROP_EMQX_IMAGE:-emqx/emqx:5.8.9}
MOSQUITTO_PORT=18830
EMQX_PORT=18831

find_binary() {
    if [ -n "${NCL_TEST_BROKER_BIN:-}" ]; then
        echo "$NCL_TEST_BROKER_BIN"
        return
    fi
    for candidate in \
        "$ROOT/build-linux/bin/test_broker" \
        "$ROOT/build/bin/ncl_test_broker" \
        "$ROOT/build/tests/ncl_test_broker.exe" \
        "$ROOT/build/bin/ncl_test_broker.exe" \
        "$ROOT/build-asan/tests/ncl_test_broker.exe"; do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return
        fi
    done
    echo ""
}

wait_port() {
    port=$1
    label=$2
    i=0
    while [ $i -lt 60 ]; do
        if (echo > "/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            echo "  $label is up on 127.0.0.1:$port"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "  $label did not come up on 127.0.0.1:$port" >&2
    return 1
}

# A TCP listener can be up before the broker is ready to serve MQTT (EMQX takes
# a few seconds), so poll with the probe mode of the test binary itself.
wait_ready() {
    port=$1
    label=$2
    i=0
    while [ $i -lt 60 ]; do
        if NCL_TEST_MQTT_BROKER="tcp://127.0.0.1:$port" \
           NCL_TEST_BROKER_PROBE=1 "$BIN" >/dev/null 2>&1; then
            echo "  $label accepts connections"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "  $label never accepted a CONNECT" >&2
    return 1
}

start_mosquitto() {
    docker rm -f ncl-interop-mosquitto >/dev/null 2>&1
    # The official image ships no config, so write one that accepts anonymous
    # clients and exec mosquitto with it.
    docker run -d --name ncl-interop-mosquitto \
        -p "127.0.0.1:$MOSQUITTO_PORT:1883" "$MOSQUITTO_IMAGE" \
        sh -c "printf 'listener 1883\nallow_anonymous true\n' \
               > /mosquitto/config/mosquitto.conf && \
               exec mosquitto -c /mosquitto/config/mosquitto.conf" >/dev/null
}

start_emqx() {
    docker rm -f ncl-interop-emqx >/dev/null 2>&1
    # EMQX allows anonymous clients until an authentication chain is configured.
    docker run -d --name ncl-interop-emqx \
        -p "127.0.0.1:$EMQX_PORT:1883" "$EMQX_IMAGE" >/dev/null
}

stop_all() {
    docker rm -f ncl-interop-mosquitto >/dev/null 2>&1
    docker rm -f ncl-interop-emqx >/dev/null 2>&1
}

run_broker() {
    name=$1
    port=$2
    printf '== %s (127.0.0.1:%s) ==\n' "$name" "$port"
    if NCL_TEST_MQTT_BROKER="tcp://127.0.0.1:$port" "$BIN"; then
        echo "  $name: PASS"
        return 0
    fi
    echo "  $name: FAIL" >&2
    return 1
}

which=${1:-both}
case "$which" in
    mosquitto|emqx|both) ;;
    *) echo "usage: $0 [mosquitto|emqx|both]" >&2; exit 2 ;;
esac

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required" >&2
    exit 2
fi

BIN=$(find_binary)
if [ -z "$BIN" ]; then
    echo "test_broker was not found: build first (build.ps1 or build-linux.sh)" >&2
    exit 2
fi
echo "test binary: $BIN"

failures=0
trap 'stop_all' EXIT INT TERM

if [ "$which" = "mosquitto" ] || [ "$which" = "both" ]; then
    start_mosquitto || exit 1
    wait_port $MOSQUITTO_PORT "mosquitto" || exit 1
    wait_ready $MOSQUITTO_PORT "mosquitto" || exit 1
    run_broker "mosquitto" $MOSQUITTO_PORT || failures=$((failures + 1))
    docker rm -f ncl-interop-mosquitto >/dev/null 2>&1
fi

if [ "$which" = "emqx" ] || [ "$which" = "both" ]; then
    start_emqx || exit 1
    wait_port $EMQX_PORT "emqx" || exit 1
    wait_ready $EMQX_PORT "emqx" || exit 1
    run_broker "emqx" $EMQX_PORT || failures=$((failures + 1))
    docker rm -f ncl-interop-emqx >/dev/null 2>&1
fi

echo
if [ $failures -eq 0 ]; then
    echo "interop: all brokers passed"
else
    echo "interop: $failures broker(s) failed"
fi
exit $failures
