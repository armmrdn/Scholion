#!/bin/bash
# Fetches third-party dependencies for Scholion.
# Run once after cloning the repo.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
THIRD_PARTY="$SCRIPT_DIR/third_party"

echo "=== Scholion dependency setup ==="

# --- Dear ImGui (v1.91.8) ---------------------------------------------------
IMGUI_VERSION="v1.91.8"
IMGUI_DIR="$THIRD_PARTY/imgui"

if [ -d "$IMGUI_DIR" ]; then
    echo "Dear ImGui already present at $IMGUI_DIR — skipping."
else
    echo "Fetching Dear ImGui $IMGUI_VERSION ..."
    git clone --depth 1 --branch "$IMGUI_VERSION" \
        https://github.com/ocornut/imgui.git "$IMGUI_DIR"
    echo "Dear ImGui ready."
fi

# --- Platform dependencies ---------------------------------------------------
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo ""
    echo "macOS detected. Checking for Homebrew dependencies..."
    if ! command -v brew &>/dev/null; then
        echo "WARNING: Homebrew not found. Install it from https://brew.sh"
        echo "Then run: brew install cmake glfw"
    else
        for pkg in cmake glfw; do
            if brew list "$pkg" &>/dev/null; then
                echo "  $pkg — installed"
            else
                echo "  $pkg — installing..."
                brew install "$pkg"
            fi
        done
    fi
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo ""
    echo "Linux detected. You may need:"
    echo "  sudo apt install cmake libglfw3-dev libgl-dev"
fi

echo ""
echo "=== Setup complete ==="
echo "Build with:"
echo "  mkdir build && cd build"
echo "  cmake .. -DCMAKE_BUILD_TYPE=Debug"
echo "  make -j\$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
echo "  ./scholion"
