#!/usr/bin/env bash
set -euo pipefail

GAME_TITLE="Black - XBE Recomp"
GAME_XBE="default.xbe"
GAME_EXE="BLACK"

cd "$(dirname "$0")"

echo ""
echo "  $GAME_TITLE"
echo "  =================="
echo ""

# Use "./build.sh debug" for the release build plus full logs
PRESET="release"
if [[ "${1:-}" =~ ^[Dd][Ee][Bb][Uu][Gg]$ ]]; then
    PRESET="debug"
fi
OUT="$PWD/out/build/$PRESET"

fail() {
    echo ""
    echo "  BUILD FAILED. Read the last message above."
    echo ""
    exit 1
}

if [[ ! -f "assets/$GAME_XBE" ]]; then
    echo "  Put the full game files in the assets folder first:"
    echo "    $PWD/assets"
    echo "  It needs $GAME_XBE with GlobData.bin and the chars, data, export, language, levels, sound and videos folders beside it (or the game .iso alongside $GAME_XBE)."
    fail
fi

# ---------------------------------------------------------------- toolchain
echo "[1/3] Toolchain"

check_tools() {
    MISSING=()
    for cmd in clang++ ld.lld cmake ninja git pkg-config; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            MISSING+=("$cmd")
        fi
    done
    if [[ ! -f /usr/include/vulkan/vulkan.h && ! -f /usr/local/include/vulkan/vulkan.h ]]; then
        MISSING+=("libvulkan-dev")
    fi
    if [[ ! -f /usr/include/shaderc/shaderc.hpp && ! -f /usr/local/include/shaderc/shaderc.hpp ]]; then
        MISSING+=("libshaderc-dev")
    fi
}

check_tools
if (( ${#MISSING[@]} > 0 )); then
    echo "      missing: ${MISSING[*]}"
    if [[ -z "${CI:-}" ]] && command -v apt-get >/dev/null 2>&1; then
        read -r -p "      Install missing packages with sudo apt-get? [y/N] " ans
        if [[ "$ans" =~ ^[Yy]$ ]]; then
            sudo apt-get update && sudo apt-get install -y \
                clang lld cmake ninja-build git pkg-config \
                libvulkan-dev libshaderc-dev \
                libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
                libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev \
                libasound2-dev libpulse-dev libudev-dev libdbus-1-dev || fail
            check_tools
        fi
    fi
    if (( ${#MISSING[@]} > 0 )); then
        echo ""
        echo "      Install the required dependencies by hand, then run this again:"
        echo "        Ubuntu/Debian: sudo apt install clang lld cmake ninja-build git pkg-config libvulkan-dev libshaderc-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev"
        fail
    fi
fi

echo "      $(clang++ --version | head -n 1)"

# ---------------------------------------------------------------- symbols
echo "[2/3] XDK symbols"
cmake --preset "$PRESET" >/dev/null || fail

if [[ -f config/game.toml ]] && grep -q '^\[symbols\]' config/game.toml; then
    echo "      config/game.toml already written"
else
    cmake --build "$OUT" --target xbe_symbols >/dev/null || fail
    if [[ ! -f "$OUT/sdk_dir.txt" ]]; then
        echo "      out/build/$PRESET/sdk_dir.txt was not written, update the SDK."
        fail
    fi
    SDK="$(head -n 1 "$OUT/sdk_dir.txt" | tr -d '\r\n')"
    mkdir -p config
    "$OUT/xbe_symbols" "assets/$GAME_XBE" "$SDK/xdk" --out config/game.toml || fail
fi

# ---------------------------------------------------------------- game
echo "[3/3] Game code"
echo "      the first build recompiles $GAME_XBE: 30 to 90 minutes on weaker systems ..."
cmake --build "$OUT" || fail

echo ""
echo "  $GAME_TITLE built."
echo "  out/build/$PRESET/$GAME_EXE"
echo ""

if [[ -n "${CI:-}" ]]; then
    exit 0
fi

read -r -p "  Play now? [y/N] " play_ans
if [[ "$play_ans" =~ ^[Yy]$ ]]; then
    (cd "$OUT" && "./$GAME_EXE")
fi
