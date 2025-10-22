#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
MOD="$ROOT_DIR/kernel/nxp_simtemp.ko"
DEV="/dev/simtemp"
SYS="/sys/class/misc/simtemp"

echo "[1/5] Build"
sudo bash $ROOT_DIR/scripts/build.sh

echo "[2/5] Load"
sudo rmmod nxp_simtemp 2>/dev/null || true
sudo insmod "$MOD"

echo "[3/5] Configure"
sleep 0.2
echo ramp    | sudo tee "$SYS/mode" >/dev/null
echo 100     | sudo tee "$SYS/sampling_ms" >/dev/null
echo 42000   | sudo tee "$SYS/threshold_mC" >/dev/null

echo "[4/5] Run CLI"
python3 "$ROOT_DIR/cli/simtemp_cli.py" --test || { echo "[FAIL] CLI test"; sudo rmmod nxp_simtemp; exit 1; }

echo "[5/5] Unload"
sudo rmmod nxp_simtemp
echo "[OK] Demo complete"
