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
MOSQUITTO_TLS_PORT=18832
MOSQUITTO_TLS_DIR=${NCL_INTEROP_TLS_DIR:-$ROOT/build-linux/interop-tls}
TLS_CERT=$ROOT/tests/data/tls_localhost_cert.pem

# Git Bash rewrites absolute paths in arguments; hand Docker real Windows paths
# and switch that rewriting off for the calls that mount directories.
host_path() {
    (cd "$1" && pwd -W 2>/dev/null) || (cd "$1" && pwd)
}

# Windows has no OpenSSL SDK here, so the TLS run uses the Linux binary built
# with NCL_WITH_TLS=1 inside a container; on Linux it runs natively.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) RUN_TLS_IN_DOCKER=1 ;;
    *) RUN_TLS_IN_DOCKER=0 ;;
esac

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

# The TLS run needs a binary built with NCL_WITH_TLS=1.
find_tls_binary() {
    for candidate in \
        "$ROOT/build-linux-tls/bin/test_broker" \
        "$ROOT/build-tls/bin/ncl_test_broker"; do
        # -f, not -x: a Linux binary built inside Docker and read back through a
        # Windows filesystem has no executable bit, yet we only exec it in a
        # container.
        if [ -f "$candidate" ]; then
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

# TLS listener on MOSQUITTO_TLS_PORT, using the certificates in
# tests/data (SAN: DNS:localhost, IP:127.0.0.1) so the client can verify them.
prepare_mosquitto_tls() {
    mkdir -p "$MOSQUITTO_TLS_DIR"
    cp "$ROOT/tests/data/tls_localhost_cert.pem" "$MOSQUITTO_TLS_DIR/server.crt"
    cp "$ROOT/tests/data/tls_localhost_key.pem" "$MOSQUITTO_TLS_DIR/server.key"
    chmod 644 "$MOSQUITTO_TLS_DIR/server.key" 2>/dev/null || true
    return 0
}

start_mosquitto_tls() {
    docker rm -f ncl-interop-mosquitto-tls >/dev/null 2>&1
    MSYS_NO_PATHCONV=1 docker run -d --name ncl-interop-mosquitto-tls \
        -p "127.0.0.1:$MOSQUITTO_TLS_PORT:8883" \
        -v "$(host_path "$MOSQUITTO_TLS_DIR"):/mosquitto/certs" "$MOSQUITTO_IMAGE" \
        sh -c "printf 'listener 8883\ncafile /mosquitto/certs/server.crt\ncertfile /mosquitto/certs/server.crt\nkeyfile /mosquitto/certs/server.key\nallow_anonymous true\n' \
               > /mosquitto/config/mosquitto.conf && \
               cp /mosquitto/certs/server.crt /mosquitto/certs/server.key /tmp/ && \
               chmod 644 /tmp/server.key && \
               sed -i 's#/mosquitto/certs/#/tmp/#g' /mosquitto/config/mosquitto.conf && \
               exec mosquitto -c /mosquitto/config/mosquitto.conf" >/dev/null
}

run_broker_tls() {
    port=$1
    tls_bin=$2

    if [ "$RUN_TLS_IN_DOCKER" = "1" ]; then
        # The repository is mounted at /work, so address the binary through it.
        container_bin="/work/${tls_bin#"$ROOT"/}"
        printf '== mosquitto+tls (ssl://host.docker.internal:%s) ==\n' "$port"
        if MSYS_NO_PATHCONV=1 docker run --rm \
                --add-host host.docker.internal:host-gateway \
                -v "$(host_path "$ROOT"):/work" -w /work \
                -e "NCL_TEST_MQTT_BROKER=ssl://host.docker.internal:$port" \
                -e "NCL_TEST_MQTT_CA=/work/tests/data/tls_localhost_cert.pem" \
                -e "NCL_TEST_MQTT_SERVER_NAME=127.0.0.1" \
                gcc:13 "$container_bin"; then
            echo "  mosquitto+tls: PASS"
            return 0
        fi
    else
        printf '== mosquitto+tls (ssl://127.0.0.1:%s) ==\n' "$port"
        if NCL_TEST_MQTT_BROKER="ssl://127.0.0.1:$port" \
           NCL_TEST_MQTT_CA="$TLS_CERT" "$tls_bin"; then
            echo "  mosquitto+tls: PASS"
            return 0
        fi
    fi
    echo "  mosquitto+tls: FAIL" >&2
    echo "  (the TLS run needs a binary built with NCL_WITH_TLS=1)" >&2
    return 1
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
    docker rm -f ncl-interop-mosquitto-tls >/dev/null 2>&1
}

run_broker() {
    name=$1
    port=$2
    scheme=${3:-tcp}
    ca=${4:-}
    printf '== %s (%s://127.0.0.1:%s) ==\n' "$name" "$scheme" "$port"
    if NCL_TEST_MQTT_BROKER="$scheme://127.0.0.1:$port" \
       NCL_TEST_MQTT_CA="$ca" "$BIN"; then
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

    BIN_TLS=$(find_tls_binary)
    if [ -z "$BIN_TLS" ]; then
        echo "== mosquitto+tls: skipped (build with NCL_WITH_TLS=1 first) =="
    else
        prepare_mosquitto_tls
        start_mosquitto_tls || exit 1
        wait_port $MOSQUITTO_TLS_PORT "mosquitto (tls)" || exit 1
        run_broker_tls $MOSQUITTO_TLS_PORT "$BIN_TLS" || failures=$((failures + 1))
        docker rm -f ncl-interop-mosquitto-tls >/dev/null 2>&1
    fi
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
