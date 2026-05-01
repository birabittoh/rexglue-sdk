#!/usr/bin/env bash
set -euo pipefail

# ── Config ──────────────────────────────────────────────────────────────────
REKAMEO="ReKameo"          # Target project directory name (sibling of SDK root)
BUILD_CONFIG="Debug"       # Debug | Release | RelWithDebInfo
PRESET="linux-amd64"       # CMake configure preset
# ────────────────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="$(dirname "$SCRIPT_DIR")"
REKAMEO_DIR="$(realpath "$SDK_ROOT/../$REKAMEO")"

if [[ ! -d "$REKAMEO_DIR" ]]; then
    echo "error: '$REKAMEO_DIR' does not exist. Fix the REKAMEO variable." >&2
    exit 1
fi

THREADS=$(nproc)
INSTALL_PREFIX="$SDK_ROOT/out/install/$PRESET"

# 1. Init submodules if any are missing
if git -C "$SDK_ROOT" submodule status | grep -q '^-'; then
    echo "==> Initializing missing submodules..."
    git -C "$SDK_ROOT" submodule update --init --recursive
fi

# 2. Configure (idempotent) + build
echo "==> Configuring ($PRESET)..."
cmake --preset "$PRESET"

echo "==> Building ($BUILD_CONFIG, -j$THREADS)..."
cmake --build --preset "${PRESET}-$(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]')" -- -j "$THREADS"

# 3. Install to staging prefix
echo "==> Installing to $INSTALL_PREFIX..."
cmake --install "$SDK_ROOT/out/build/$PRESET" --config "$BUILD_CONFIG" --prefix "$INSTALL_PREFIX"

# 4. Wipe old SDK, copy fresh (skip .sdk-version)
TARGET="$REKAMEO_DIR/sdk"
echo "==> Deploying to $TARGET..."
rm -rf "$TARGET"
mkdir -p "$TARGET"

find "$INSTALL_PREFIX" -mindepth 1 -maxdepth 1 ! -name '.sdk-version' -exec cp -r {} "$TARGET/" \;

echo "==> Done. SDK deployed to $TARGET"
