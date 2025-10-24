#!/bin/bash
# build.sh - Build kernel module and GUI
# Usage: ./scripts/build.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$(realpath "$0")")/.." && pwd)"
KERNEL_DIR="$ROOT_DIR/kernel"
GUI_DIR="$ROOT_DIR/gui"
GUI_BUILD_DIR="$GUI_DIR/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
KVER=$(uname -r)
KDIR="/lib/modules/$KVER/build"

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
error() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

info "Building nxp_simtemp kernel module for kernel $KVER"

if [[ ! -d "$KDIR" ]]; then
    error "Kernel headers not found at $KDIR
Install with: sudo apt install linux-headers-$(uname -r)"
fi

make -C "$KDIR" M="$KERNEL_DIR" modules

KMOD_PATH="$KERNEL_DIR/nxp_simtemp.ko"

info "Configuring GUI build directory ($BUILD_TYPE)"

GUI_BIN=""

if ! command -v cmake >/dev/null 2>&1; then
    warn "cmake not found; skipping GUI build"
else
    cmake -S "$GUI_DIR" -B "$GUI_BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build "$GUI_BUILD_DIR" --config "$BUILD_TYPE"

    GUI_BIN_CANDIDATES=(
        "$GUI_BUILD_DIR/simtemp_gui"
        "$GUI_BUILD_DIR/$BUILD_TYPE/simtemp_gui"
    )

    for bin_path in "${GUI_BIN_CANDIDATES[@]}"; do
        if [[ -x "$bin_path" ]]; then
            GUI_BIN="$bin_path"
            break
        fi
    done

    if [[ -z "$GUI_BIN" ]]; then
        warn "GUI binary not found in build directory"
    fi
fi

printf '[OK] Build complete:\n'
printf '     %s\n' "$KMOD_PATH"

if [[ -n "$GUI_BIN" ]]; then
    printf '     %s\n' "$GUI_BIN"
fi
