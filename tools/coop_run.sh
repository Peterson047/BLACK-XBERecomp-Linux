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
    for pid in "${HOST_PID:-}" "${GUEST_PID:-}"; do
        [[ -n "$pid" ]] && kill -TERM -- "-$pid" 2>/dev/null
    done
    sleep 1
    for pid in "${HOST_PID:-}" "${GUEST_PID:-}"; do
        [[ -n "$pid" ]] && kill -KILL -- "-$pid" 2>/dev/null
    done
    [[ -n "${GUEST_XVFB:-}" ]] && kill "$GUEST_XVFB" 2>/dev/null
    [[ -n "${HOST_XVFB:-}" ]] && kill "$HOST_XVFB" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

echo "run dir: $RUN_DIR  duration: ${DURATION}s  port: $PORT"

# The window is the only thing that needs a display; the game itself is happy
# on a virtual one. Two Xvfb servers keep the instances apart.
#
# The displays are not fixed: Xvfb picks a free one and writes its number to
# the file descriptor. A fixed display is shared by two runs that happen at
# the same time, and when one of them ends it takes the X server of the other
# with it: the instance loses its window and dies.
start_instance() {
    local name="$1"; shift
    local display_file="$RUN_DIR/display.$name"
    Xvfb -displayfd 3 -screen 0 1280x720x24 >/dev/null 2>&1 3>"$display_file" &
    local xvfb_pid=$!
    local display=""
    for _ in $(seq 1 50); do
        local number
        number="$(cat "$display_file" 2>/dev/null)"
        if [[ -n "$number" ]]; then
            display=":$number"
            break
        fi
        sleep 0.1
    done
    if [[ -z "$display" ]]; then
        echo "no display for $name" >&2
        return 1
    fi
    # The game relaunches itself, so the instance gets its own group and the
    # whole group has to go when the run ends.
    setsid env DISPLAY="$display" "$BIN" "$@" >"$RUN_DIR/$name.out" 2>&1 &
    local game_pid=$!
    echo "$xvfb_pid $game_pid"
}

read -r HOST_XVFB HOST_PID <<<"$(start_instance host --coop --host "$PORT" --name "$HOST_NAME")"
sleep 2
read -r GUEST_XVFB GUEST_PID <<<"$(start_instance guest --coop --connect "127.0.0.1:$PORT" --name "$GUEST_NAME")"

sleep "$DURATION"

# The instances get a polite shutdown, so they print their summary.
kill -TERM -- "-$HOST_PID" "-$GUEST_PID" 2>/dev/null
sleep 2
kill -KILL -- "-$HOST_PID" "-$GUEST_PID" 2>/dev/null
kill "$HOST_XVFB" "$GUEST_XVFB" 2>/dev/null
wait 2>/dev/null

# The logger mirrors every line to stderr, so each instance's own capture is
# the complete and correctly attributed log. Identifying the instances by the
# log file's modification time is a race, so they are told apart by content:
# the host says it started a server, the guest says it sent a join request.
for who in host guest; do
    if [[ -s "$RUN_DIR/$who.out" ]]; then
        cp "$RUN_DIR/$who.out" "$RUN_DIR/$who.log"
    fi
done

if ! grep -q "Server started on port" "$RUN_DIR/host.out" 2>/dev/null; then
    echo "the host did not report a server; see $RUN_DIR/host.out"
fi
if ! grep -q "Sent JoinRequest" "$RUN_DIR/guest.out" 2>/dev/null; then
    echo "the guest did not report a join request; see $RUN_DIR/guest.out"
fi

echo
echo "-- network, multiplayer and coop lines --"
for who in host guest; do
    echo "### $who"
    grep -hiE "NetworkManager|MultiplayerManager|Coop:|coop:" "$RUN_DIR/$who.out" 2>/dev/null | tail -25 | sed 's/^/    /'
done
echo
echo "-- the two sides compared --"
if python3 tests/coop/assert_coop_log.py "$RUN_DIR/host.out" "$RUN_DIR/guest.out"; then
    echo "the two instances agree"
else
    echo "the two instances do not agree yet"
fi
echo
echo "run dir kept at $RUN_DIR"
