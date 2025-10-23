# TESTPLAN.md — NXP Simulated Temperature Sensor (`nxp_simtemp`)

**Author:** José Giuseppe Pia Figueroa  
**Date:** October 2025  
**Scope:** Functional + robustness tests for kernel module, sysfs, data path, and CLI.

---

## 1. Environment

- OS: Ubuntu 22.04+ (or equivalent)
- Kernel headers installed: `linux-headers-$(uname -r)`
- Python 3.8+
- Repo layout: `kernel/`, `cli/`, `scripts/`

### Pre-check
```bash
./scripts/build.sh
sudo rmmod nxp_simtemp 2>/dev/null || true
sudo insmod kernel/nxp_simtemp.ko
ls -l /dev/simtemp
```
Pass if `/dev/simtemp` exists.

---

## 2. Test Matrix (modes × sampling)

| Test ID |  Mode   | Sampling_ms | Threshold_mC |
|---------|---------|-------------|--------------|
| M1      | normal  |    100      |   42000      |
| M2      | noisy   |    100      |   42000      |
| M4      | ramp    |     50      |   26000      |

Each test below may be executed with one or more matrix rows.

---

## 3. Tests

### T1 — Load/Unload Clean
**Goal:** No warnings, nodes present/removed properly.  
**Steps:**
```bash
./scripts/build.sh
sudo rmmod nxp_simtemp 2>/dev/null || true
sudo insmod kernel/nxp_simtemp.ko
ls -l /dev/simtemp
sudo rmmod nxp_simtemp
dmesg | tail -n 50
```
**Pass:** `/dev/simtemp` appears on load, disappears on unload; no WARN/OOPS/leaks in `dmesg`.

---

### T2 — Periodic Read (blocking)
**Goal:** Reader blocks then gets data; rates roughly match sampling.  
**Steps:**
```bash
sudo insmod kernel/nxp_simtemp.ko
echo 100 | sudo tee /sys/class/misc/simtemp/sampling_ms >/dev/null
sudo -E python3 cli/simtemp_cli.py | head -n 20
```
**Pass:** ~10±2 lines/second; timestamps monotonic; temps valid.

---

### T3 — Non-blocking Read
**Goal:** `O_NONBLOCK` returns `-EAGAIN` when empty.  
**Steps:**
```bash
python3 - <<'PY'
import os, fcntl
fd=os.open("/dev/simtemp", os.O_RDONLY|os.O_NONBLOCK)
try:
    os.read(fd, 24)
except BlockingIOError as e:
    print("EAGAIN OK")
else:
    print("Unexpected data or error")
os.close(fd)
PY
```
**Pass:** Prints `EAGAIN OK` (assuming read before first sample).

---

### T4 — poll() New Data
**Goal:** `poll()` wakes on new sample (`POLLIN`).  
**Steps:**
```bash
python3 cli/simtemp_cli.py | head -n 5
```
**Pass:** Lines appear without manual reads; no timeouts.

---

### T5 — Threshold Event (POLLPRI / flag)
**Goal:** Alert when crossing threshold.  
**Steps:**
```bash
echo ramp  | sudo tee /sys/class/misc/simtemp/mode >/dev/null
echo 100   | sudo tee /sys/class/misc/simtemp/sampling_ms >/dev/null
echo 26000 | sudo tee /sys/class/misc/simtemp/threshold_mC >/dev/null
sudo -E python3 cli/simtemp_cli.py --test
```
**Pass:** CLI exits `0` with `TEST: PASS (threshold event)` within ~2 periods.

---

### T6 — Sysfs Validation & Error Paths
**Goal:** Validate ranges and error handling.  
**Steps:**
```bash
echo -n > /sys/class/misc/simtemp/sampling_ms || true
echo -123 | sudo tee /sys/class/misc/simtemp/sampling_ms || true
echo 0     | sudo tee /sys/class/misc/simtemp/sampling_ms || true
echo 1     | sudo tee /sys/class/misc/simtemp/sampling_ms || true
echo bogus | sudo tee /sys/class/misc/simtemp/mode || true
```
**Pass:** Invalid writes fail with non-zero exit and `-EINVAL` in `dmesg`; valid ones apply.

---

### T7 — Concurrency (Reader + Writer)
**Goal:** No deadlocks with concurrent read and sysfs writes.  
**Steps (parallel terminals):**
- **Term A:** `sudo -E python3 cli/simtemp_cli.py`
- **Term B:** loop changing attributes:
```bash
for m in normal noisy ramp; do echo $m | sudo tee /sys/class/misc/simtemp/mode; sleep 0.2; done
for s in 50 100 200; do echo $s | sudo tee /sys/class/misc/simtemp/sampling_ms; sleep 0.2; done
```
**Pass:** CLI keeps printing; no kernel stalls/warnings; stats increment.

---

### T8 — Stats Sanity
**Goal:** Counters look reasonable.  
**Steps:**
```bash
cat /sys/class/misc/simtemp/stats
```
**Pass:** `updates` increases with time; `alerts` increases on threshold tests; `last_error` remains 0 in normal runs.

---

### T9 — Clean Unload with Active Readers
**Goal:** Unload does not hang with blocked readers.  
**Steps:**
- Start CLI (blocking).  
- In another terminal: `sudo rmmod nxp_simtemp`  
**Pass:** Module unloads cleanly (reader exits or errors gracefully), no OOPS.

---

## 4. Automation

Run the integrated demo (compiles, configures, test-mode, unload):
```bash
./scripts/run_demo.sh
```
**Pass:** Exit code `0`, prints `[OK] Demo complete`.

---

## 5. Logging & Evidence (optional)

- Save logs:
```bash
dmesg -w | ts '[%Y-%m-%d %H:%M:%S]' | tee /tmp/simtemp_klog.txt
sudo -E python3 cli/simtemp_cli.py | tee /tmp/simtemp_cli.txt
```
- Keep artifacts in `docs/test_logs/` for submission.

---

## 6. Risks & Known Limitations

- High-rate sampling (≤ 1 ms) may stress workqueue; not a real-time guarantee.
- Sysfs is not atomic; for batch updates prefer ioctl (future work).
- CLI assumes record size `24` bytes (`=Q i I`); mismatch will fail reads.

---

## 7. Exit Criteria

- All T1–T8 pass on at least one matrix row (M3 or M4 recommended).
- `run_demo.sh` exits `0` on two consecutive runs.
