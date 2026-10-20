#!/usr/bin/env bash
# Runs two instances of the game headless: one hosts, one joins, both load
# Veblensk through the direct map launch. The game logs are the observation
# channel for the co-op behaviour, so they are copied out with a prefix.
#
# Usage:
#   tools/coop_run.sh [seconds] [port]
#
# The instances run under Xvfb so they do not take over the desktop. The
# binary must be built first: cmake --build out/build/release
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
BIN="out/build/release/BLACK"
LOGDIR="out/build/release/logs"

DURATION="${1:-60}"
PORT="${2:-15099}"
HOST_NAME="CoopHost"
GUEST_NAME="CoopGuest"

if [[ ! -x "$BIN" ]]; then
    echo "no binary at $BIN; build it first: cmake --build out/build/release"
    exit 1
fi

RUN_DIR="$(mktemp -d /tmp/coop-run-XXXXXX)"
cleanup() {
    [[ -n "${HOST_PID:-}" ]] && kill "$HOST_PID" 2>/dev/null
    [[ -n "${GUEST_PID:-}" ]] && kill "$GUEST_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT

echo "run dir: $RUN_DIR  duration: ${DURATION}s  port: $PORT"

# The window is the only thing that needs a display; the game itself is happy
# on a virtual one. Two Xvfb servers keep the instances apart.
start_instance() {
    local name="$1"; shift
    local display="$1"; shift
    Xvfb "$display" -screen 0 1280x720x24 >/dev/null 2>&1 &
    local xvfb_pid=$!
    sleep 1
    DISPLAY="$display" "$BIN" "$@" >"$RUN_DIR/$name.out" 2>&1 &
    local game_pid=$!
    echo "$xvfb_pid $game_pid"
}

read -r HOST_XVFB HOST_PID <<<"$(start_instance host ":77" --host "$PORT" --name "$HOST_NAME")"
sleep 2
read -r GUEST_XVFB GUEST_PID <<<"$(start_instance guest ":78" --connect "127.0.0.1:$PORT" --name "$GUEST_NAME")"

sleep "$DURATION"

kill "$HOST_PID" "$GUEST_PID" 2>/dev/null
sleep 1
kill "$HOST_XVFB" "$GUEST_XVFB" 2>/dev/null
wait 2>/dev/null

# the game rotates its log file, so pick the newest two and label them
mapfile -t LOGS < <(ls -t "$LOGDIR"/*.log 2>/dev/null | head -2)
if [[ ${#LOGS[@]} -lt 2 ]]; then
    echo "no game logs found in $LOGDIR"
    exit 1
fi

HOST_LOG="${LOGS[0]}"
GUEST_LOG="${LOGS[1]}"
cp "$HOST_LOG" "$RUN_DIR/host.log"
cp "$GUEST_LOG" "$RUN_DIR/guest.log"

echo
echo "== host log:  $HOST_LOG"
echo "== guest log: $GUEST_LOG"
echo
echo "-- network and multiplayer lines --"
for who in host guest; do
    echo "### $who"
    grep -hiE "NetworkManager|MultiplayerManager|Coop|coop" "$RUN_DIR/$who.log" | tail -20 | sed 's/^/    /'
done
echo
echo "run dir kept at $RUN_DIR"
