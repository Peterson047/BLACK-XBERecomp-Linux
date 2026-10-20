#!/usr/bin/env bash
# Development gate for the co-op work.
#
# The game assets (assets/default.xbe) are not part of this repository, so the
# game binary cannot be built here. These are the checks that do work without
# them:
#
#   1. syntax/compile check of the game-side translation units
#   2. build and run of the standalone C++ tests (real UDP on localhost)
#   3. the python end-to-end suite
#
# Usage:
#   tools/dev_check.sh            # everything
#   tools/dev_check.sh syntax     # only the compile check
#   tools/dev_check.sh cpp        # only the C++ tests
#   tools/dev_check.sh e2e        # only the python suite
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

CXX="${CXX:-clang++}"
IMGUI="${IMGUI_DIR:-}"
if [[ -z "$IMGUI" ]]; then
    for candidate in \
        "$ROOT/out/build/release/_deps/imgui-src" \
        "$ROOT/out/build/debug/_deps/imgui-src" \
        "$HOME/Downloads/BLACK-XBERecomp-main/out/build/release/_deps/imgui-src"
    do
        if [[ -f "$candidate/imgui.h" ]]; then IMGUI="$candidate"; break; fi
    done
fi

GAME_SOURCES=(
    src/game/multiplayer.cpp
    src/game/hooks.cpp
    src/game/hud.cpp
    src/game/multiplayer_camera.cpp
    src/game/multiplayer_debug.cpp
    src/net/network.cpp
)
# coop sources are added as they appear
for extra in src/game/coop_*.cpp src/net/coop_*.cpp; do
    [[ -f "$extra" ]] && GAME_SOURCES+=("$extra")
done

fail=0

run_syntax() {
    echo "== syntax check =="
    if [[ ! -f generated/game/image_symbols.h ]]; then
        echo "  SKIP: generated/game/image_symbols.h is missing (needs a build with the assets)"
        return 0
    fi
    if [[ -z "$IMGUI" ]]; then
        echo "  SKIP: imgui headers not found; set IMGUI_DIR to the imgui source directory"
        return 0
    fi
    local failed=0
    for src in "${GAME_SOURCES[@]}"; do
        [[ -f "$src" ]] || continue
        if "$CXX" -std=c++20 -fsyntax-only -Isrc -Isdk/runtime -Igenerated/game -I"$IMGUI" \
            -Wno-nontrivial-memcall "$src" 2>/tmp/dev_check_err.txt; then
            echo "  ok   $src"
        else
            echo "  FAIL $src"
            sed -n '1,25p' /tmp/dev_check_err.txt | sed 's/^/       /'
            failed=1
        fi
    done
    [[ $failed -eq 0 ]] || fail=1
    return 0
}

run_cpp() {
    echo "== C++ standalone tests =="
    local tests=(
        "tests/test_multiplayer_network.cpp|src/net/network.cpp"
        "tests/test_coop_protocol.cpp|src/net/coop_protocol.cpp"
    )
    for entry in "${tests[@]}"; do
        IFS='|' read -r -a parts <<< "$entry"
        local test="${parts[0]}"
        [[ -f "$test" ]] || continue
        local missing=0
        for extra in "${parts[@]:1}"; do
            [[ -f "$extra" ]] || missing=1
        done
        if [[ $missing -eq 1 ]]; then
            echo "  SKIP $test (a source it needs is not there yet)"
            continue
        fi
        local out="/tmp/$(basename "${test%.cpp}")"
        local link=()
        for extra in "${parts[@]:1}"; do link+=("$extra"); done
        if "$CXX" -std=c++20 -Isrc -Isdk/runtime "$test" "${link[@]}" \
            sdk/runtime/core/log.cpp -pthread -o "$out" 2>/tmp/dev_check_err.txt; then
            if "$out" >/tmp/dev_check_out.txt 2>&1; then
                echo "  ok   $test"
                tail -1 /tmp/dev_check_out.txt | sed 's/^/       /'
            else
                echo "  FAIL $test (ran, exited non-zero)"
                tail -20 /tmp/dev_check_out.txt | sed 's/^/       /'
                fail=1
            fi
        else
            echo "  FAIL $test (did not build)"
            sed -n '1,25p' /tmp/dev_check_err.txt | sed 's/^/       /'
            fail=1
        fi
    done
    return 0
}

run_e2e() {
    echo "== python e2e suite =="
    if [[ ! -f tests/e2e/runner.py ]]; then
        echo "  SKIP: tests/e2e/runner.py not found"
        return 0
    fi
    if python3 tests/e2e/runner.py >/tmp/dev_check_e2e.txt 2>&1; then
        echo "  ok   tests/e2e/runner.py"
        grep -E "^(Total Tests Executed|Passed|Failed):" /tmp/dev_check_e2e.txt | sed 's/^/       /'
    else
        echo "  FAIL tests/e2e/runner.py"
        grep -E "^(Total Tests Executed|Passed|Failed):" /tmp/dev_check_e2e.txt | sed 's/^/       /'
        grep -E "^  - " /tmp/dev_check_e2e.txt | sed 's/^/       /' | head -20
        fail=1
    fi
    return 0
}

run_coop() {
    echo "== co-op log checker =="
    if [[ ! -f tests/coop/test_assert_coop_log.py ]]; then
        echo "  SKIP: the co-op log checker is not there yet"
        return 0
    fi
    if python3 tests/coop/test_assert_coop_log.py >/tmp/dev_check_coop.txt 2>&1; then
        echo "  ok   the log checker catches what it should"
    else
        echo "  FAIL the log checker"
        tail -20 /tmp/dev_check_coop.txt | sed 's/^/       /'
        fail=1
    fi

    # The two instance gate only makes sense once the co-op code is in the
    # game. Until then there is nothing for the instances to agree about.
    if [[ ! -f src/game/coop_world.cpp ]]; then
        echo "  SKIP: the two instance gate, the co-op code is not in the game yet"
        return 0
    fi
    if [[ ! -x out/build/release/BLACK ]]; then
        echo "  SKIP: the two instance gate, the game is not built"
        return 0
    fi
    echo "== two instance gate =="
    if ! tools/coop_run.sh "${COOP_RUN_SECONDS:-45}" "${COOP_RUN_PORT:-15199}" >/tmp/dev_check_run.txt 2>&1; then
        echo "  FAIL the two instances did not run"
        tail -20 /tmp/dev_check_run.txt | sed 's/^/       /'
        fail=1
        return 0
    fi
    local run_dir
    run_dir="$(grep -o '/tmp/coop-run-[A-Za-z0-9]*' /tmp/dev_check_run.txt | head -1)"
    if [[ -z "$run_dir" || ! -f "$run_dir/host.out" || ! -f "$run_dir/guest.out" ]]; then
        echo "  SKIP: the two instance gate, the run produced no logs"
        return 0
    fi
    if python3 tests/coop/assert_coop_log.py "$run_dir/host.out" "$run_dir/guest.out"; then
        echo "  ok   the two instances agree"
    else
        echo "  FAIL the two instances disagree"
        fail=1
    fi
    return 0
}

case "${1:-all}" in
    syntax) run_syntax ;;
    cpp)    run_cpp ;;
    e2e)    run_e2e ;;
    coop)   run_coop ;;
    all)    run_syntax; run_cpp; run_e2e; run_coop ;;
    *)      echo "usage: $0 [all|syntax|cpp|e2e|coop]"; exit 2 ;;
esac

if [[ $fail -ne 0 ]]; then
    echo
    echo "DEV CHECK FAILED"
    exit 1
fi
echo
echo "DEV CHECK OK"
