#!/bin/bash
# build.sh - Build kernel module (and future CLI if needed)
# Usage: ./scripts/build.sh

set -e

# Go one level up from scripts/
ROOT_DIR="$(cd "$(dirname "$(realpath "$0")")/.." && pwd)"
KERNEL_DIR="$ROOT_DIR/kernel"
KVER=$(uname -r)
KDIR="/lib/modules/$KVER/build"

echo "[INFO] Building nxp_simtemp kernel module for kernel $KVER"

if [ ! -d "$KDIR" ]; then
    echo "[ERROR] Kernel headers not found at $KDIR"
    echo "Install with: sudo apt install linux-headers-$(uname -r)"
    exit 1
fi

make -C "$KDIR" M="$KERNEL_DIR" modules

echo "[OK] Build complete:"
echo "     $KERNEL_DIR/nxp_simtemp.ko"
