# NXP Simulated Temperature Sensor (`nxp_simtemp`)

**Author:** José Giuseppe Pia Figueroa  
**Version:** 1.0 (kernel module)  
**Repo Tag:** v1.1  
**Date:** October 2025  

A small Linux kernel module and user-space CLI that simulate a virtual temperature sensor.  
Developed as part of the *NXP Systems Software Engineer Challenge*.

---

## 1. Overview

The project implements a complete “**Virtual Sensor + Alert Path**” flow:

- **Kernel module (`nxp_simtemp.ko`)**
  - Simulates periodic temperature samples (normal, noisy, or ramp modes).
  - Exposes data through `/dev/simtemp`.
  - Supports blocking `read()` and `poll()` for new data or threshold alerts.
  - Configuration via **sysfs** (`sampling_ms`, `threshold_mC`, `mode`, `stats`).

- **User-space CLI (`cli/simtemp_cli.py`)**
  - Reads live samples from `/dev/simtemp`.
  - Configures sysfs parameters.
  - Includes a `--test` mode to verify threshold alerts.
- **Desktop GUI (`gui/`)**
  - Qt-based charting dashboard for live visualization.
  - Allows adjusting sampling period, threshold, and mode from sysfs.
  - Provides alert acknowledgment, stats pop-up, and simulation start/stop controls.

- **Automation scripts (`scripts/`)**
  - `build.sh` – builds the kernel module.
  - `run_demo.sh` – builds, loads, configures, runs the CLI test, and unloads the module.

---

## 2. Directory Structure

```
nxp_simtemp/
├── kernel/               # kernel module sources
│   ├── nxp_simtemp.c
│   ├── nxp_simtemp.h
│   ├── Makefile
│   └── ...
├── gui/
│   ├── CMakeLists.txt    # Qt desktop build
│   └── main.cpp          # GUI source
├── cli/
│   └── simtemp_cli.py    # Python CLI
├── scripts/
│   ├── build.sh
│   └── run_demo.sh
└── docs/
    ├── README.md
    ├── DESIGN.md
    └── TESTPLAN.md
```

---

## 3. Build Instructions

### Requirements
- Ubuntu 22.04+ or any modern Linux distribution.
- Kernel headers installed:
  ```bash
  sudo apt install linux-headers-$(uname -r)
  ```
- Python 3.8+ (for CLI).
- CMake 3.16+ and Qt 5.15+/6.x development packages (for GUI, optional):
  ```bash
  sudo apt install qtbase5-dev qtcharts5-dev cmake
  ```

### Build
From the project root:
```bash
./scripts/build.sh
```
The compiled module appears at:
```
kernel/nxp_simtemp.ko
```

### GUI Build (optional)
To build the Qt dashboard:
```bash
cmake -S gui -B gui/build
cmake --build gui/build
```
The executable will be at `gui/build/simtemp_gui`.

---

## 4. Run Demo

Run the full test (build → load → configure → test → unload):

```bash
./scripts/run_demo.sh
```

Example output:
```
[1/5] Build
[2/5] Load
[3/5] Configure
[4/5] Run CLI test
2025-10-21T18:20:04 temp=25.123C alert=0
...
TEST: PASS (threshold event)
[5/5] Unload
[OK] Demo complete
```

---

## 5. Manual Usage

### Load Module
```bash
sudo insmod kernel/nxp_simtemp.ko
```

### Sysfs Configuration
```bash
cd /sys/class/misc/simtemp
echo ramp    | sudo tee mode
echo 100     | sudo tee sampling_ms
echo 42000   | sudo tee threshold_mC
cat stats
```

### Run CLI Manually
```bash
sudo -E python3 cli/simtemp_cli.py --sampling-ms 100 --threshold-mC 42000 --mode ramp
```

Output:
```
1697908812345678 temp=25.123C alert=0
1697908813345678 temp=25.246C alert=0
...
```

### Launch GUI Monitor
```bash
sudo gui/build/simtemp_gui
```
- Requires the module to be loaded and readable sysfs entries under `/sys/class/misc/simtemp`.
- Root privileges may be necessary for updating sysfs attributes; alternatively adjust permissions via `udev`.

---

## 6. GUI Live Monitor

- Plots live temperatures using Qt Charts with automatic axis scaling.
- Alert indicator latches red on threshold crossings until “Reset Alert” is pressed.
- Configures `sampling_ms`, `threshold_mC`, and `mode` directly through sysfs.
- “Print Stats” fetches kernel counters (`total_samples`, `threshold_crossings`).
- “Stop/Start Simulation” toggles data reads from `/dev/simtemp` without unloading the module.

---

## 7. Unload Module
```bash
sudo rmmod nxp_simtemp
```

---

## 8. Notes

- `poll()` wakes on:
  - `POLLIN | POLLRDNORM` → new data available.
  - `POLLPRI` → threshold crossed.
- The module version remains `1.0`; repository tag `v1.1` reflects addition of CLI and scripts.
- All sysfs writes require **root privileges** (`sudo`).

---

## 9. Next Steps (Optional Enhancements)

- Add data export (CSV or JSON) from GUI/CLI.
- Implement unit tests for CLI parsing and event timing.
- Add support for `ioctl()` batch configuration.
- Provide QEMU overlay and DTS for full virtual platform test.

---

## 10. License

© 2025 José Giuseppe Pia Figueroa  
This project is provided for educational and technical demonstration purposes.
