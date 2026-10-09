#!/usr/bin/env bash
#
# tools/run-dev.sh — Kill, rebuild, and launch GitBolt for a fast
# inner-loop development cycle.
#
# Usage:
#   tools/run-dev.sh [REPO_PATH]
#
#   REPO_PATH  Optional path to a Git repository to open on launch.
#              If omitted, GitBolt starts on its Dashboard view.
#
# Examples:
#   tools/run-dev.sh                    # Start fresh on the dashboard
#   tools/run-dev.sh .                  # Open the current directory
#   tools/run-dev.sh ~/projects/linux   # Open an arbitrary repo
#
# What it does (in order):
#   1. Make sure Homebrew's tools are on PATH (so this works from any shell)
#   2. Kill any running GitBolt instance (including orphaned ones). The
#      single-instance lock that `kill -9` leaves behind names a dead
#      PID, so the next launch reclaims it by itself.
#   3. Configure CMake on first use
#   4. Incrementally build the Debug target with ninja
#   5. Launch GitBolt via `open -n --args` so it goes through Launch
#      Services (gets a Dock icon, shows up in Cmd-Tab, etc.)
#
# Exit codes:
#   0 on success
#   1 if the build fails
#   2 if the binary can't be found after the build
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Locate the project root (the directory containing CMakeLists.txt)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
APP_BUNDLE="$BUILD_DIR/src/app/GitBolt.app"
BINARY="$APP_BUNDLE/Contents/MacOS/GitBolt"

# ---------------------------------------------------------------------------
# 1. Ensure Homebrew PATH is set — cmake, qt, libgit2 all live there
# ---------------------------------------------------------------------------
if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
fi

# Qt is keg-only — it won't be on PATH by default
if [[ -d /opt/homebrew/opt/qt ]]; then
    export CMAKE_PREFIX_PATH="/opt/homebrew/opt/qt:${CMAKE_PREFIX_PATH:-}"
elif [[ -d /usr/local/opt/qt ]]; then
    export CMAKE_PREFIX_PATH="/usr/local/opt/qt:${CMAKE_PREFIX_PATH:-}"
fi

# ---------------------------------------------------------------------------
# 2. Kill any running GitBolt instance
# ---------------------------------------------------------------------------
echo "==> Stopping any running GitBolt instances..."
pkill -9 -f "GitBolt.app/Contents/MacOS/GitBolt" 2>/dev/null || true
sleep 0.5

# ---------------------------------------------------------------------------
# 3. Ensure the build directory exists and is configured
# ---------------------------------------------------------------------------
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "==> Configuring CMake (first build)..."
    cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
          -DCMAKE_BUILD_TYPE=Debug \
          -G Ninja
fi

# ---------------------------------------------------------------------------
# 4. Incremental build
# ---------------------------------------------------------------------------
echo "==> Building GitBolt..."
if ! cmake --build "$BUILD_DIR" --parallel; then
    echo "!!  Build failed." >&2
    exit 1
fi

if [[ ! -x "$BINARY" ]]; then
    echo "!!  Expected binary not found at $BINARY" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 5. Launch via Launch Services
# ---------------------------------------------------------------------------
REPO_PATH="${1:-}"
if [[ -n "$REPO_PATH" ]]; then
    # Resolve to an absolute path so GitBolt gets something canonical
    REPO_PATH="$(cd "$REPO_PATH" 2>/dev/null && pwd)" || {
        echo "!!  $1 is not a directory" >&2
        exit 2
    }
    echo "==> Launching GitBolt with repository: $REPO_PATH"
    open -n "$APP_BUNDLE" --args "$REPO_PATH"
else
    echo "==> Launching GitBolt (no repository)..."
    open -n "$APP_BUNDLE"
fi

echo "==> Done."
