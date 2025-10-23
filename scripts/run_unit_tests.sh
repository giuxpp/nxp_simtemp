#!/usr/bin/env bash
# run_unit_tests.sh - Build kernel, fetch/build GoogleTest harness, and execute unit tests.

set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    exec sudo --preserve-env=PATH "$0" "$@"
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
KERNEL_KO="$ROOT_DIR/kernel/nxp_simtemp.ko"
TEST_BUILD_DIR="$ROOT_DIR/tests/build"
TEST_BIN="$TEST_BUILD_DIR/simtemp_tests"

cleanup() {
    local status=$?
    if lsmod | awk '{print $1}' | grep -qx "nxp_simtemp"; then
        rmmod nxp_simtemp || true
    fi
    exit $status
}
trap cleanup EXIT

echo "[1/4] Building kernel module"
bash "$ROOT_DIR/scripts/build.sh"

echo "[2/4] Reloading kernel module"
rmmod nxp_simtemp 2>/dev/null || true
insmod "$KERNEL_KO"

echo "[3/4] Configuring GoogleTest build"
cmake -S "$ROOT_DIR/tests" -B "$TEST_BUILD_DIR"
cmake --build "$TEST_BUILD_DIR"

echo "[4/4] Running unit tests"
"$TEST_BIN"

echo "[OK] All unit tests completed"
