# AI_NOTES.md — NXP Simulated Temperature Sensor (`nxp_simtemp`)

**Author:** José Giuseppe Pia Figueroa  
**Date:** October 2025  

---

## 1. Tools Used

- **ChatGPT (GPT‑5)** — used extensively for design discussions, feature breakdown, code guidance, and documentation generation.  
- **Codex CLI (GPT‑5 coding agent)** — handled structured coding tasks, file edits, and build/test guidance directly in the repo.  
- **Cursor IDE (AI‑powered editor)** — used for in‑IDE completions, quick reviews, inline code explanations, and small refactors.  

---

## 2. Scope of AI Assistance

| Area | How AI was used | Human validation |
|------|-----------------|------------------|
| **Architecture & Design** | Consulted ChatGPT for system structure (kernel module, workqueue, poll, sysfs) and synchronization model. | Manually verified all decisions against official kernel documentation and reference drivers. |
| **Kernel Module Implementation** | Iteratively generated and refined code for `nxp_simtemp.c`, including `read()`, `poll()`, and timer/work logic. | Every snippet was reviewed, compiled, and runtime‑tested. Adjusted types, flags, and synchronization manually. |
| **Python CLI** | Used ChatGPT to scaffold the `poll()` + `sysfs` script, and Cursor for quick syntax and error feedback. | Verified with real `/dev/simtemp` device; adjusted sysfs permissions and mappings. |
| **Qt GUI** | Leveraged ChatGPT for Qt Widgets/Charts layout, signal wiring, and sysfs UX (alert reset, stats, simulation toggle). | Reviewed moc output, built locally with Qt5, and exercised every control against a running module. |
| **Unit Testing (GoogleTest)** | Worked with Codex to design test coverage, generate scaffolding, and integrate FetchContent-based builds. | Reviewed assertions manually; executed `sudo ./tests/build/simtemp_tests` and iterated until results matched kernel behavior. |
| **Shell Scripts** | Asked ChatGPT to design `build.sh` and `run_demo.sh` layouts (robust paths, automation flow). | Executed locally; validated file paths, environment detection, and permissions. |
| **Documentation** | Drafted README, DESIGN, and TESTPLAN structure and text with ChatGPT. | Rewrote/edited to ensure accuracy and consistency with actual implementation. |

---

## 3. Validation Notes

- The entire system was **built and validated manually** on:
  ```
  Linux 6.8.0-64-generic #67-Ubuntu SMP PREEMPT_DYNAMIC x86_64 GNU/Linux
  Linux Mint 22 (based on Ubuntu 24.04, kernel headers installed)
  ```
- Kernel module compiled cleanly (`make -C /lib/modules/.../build modules`) and passed `run_demo.sh` end‑to‑end test.  
- GUI compiled via `cmake -S gui -B gui/build && cmake --build gui/build` (Qt 5.15) and validated for sysfs read/write, alert reset, stats, and simulation toggle.
- GoogleTest suite built with `cmake -S tests -B tests/build && cmake --build tests/build` and executed as root to cover sysfs writes, alert flags, poll semantics, and stress scenarios.
- Added `scripts/run_unit_tests.sh` to automate kernel rebuild, module reload, GoogleTest build, and execution.
- Verified all sysfs attributes and polling behavior manually.  
- AI‑generated code never committed blindly — all logic was checked for race conditions, API correctness, and coding style.  
- Cursor’s inline completions were used for **productivity only**, not for decision‑making.

---

## 4. Reflection

AI tools (ChatGPT + Cursor) **significantly accelerated** prototyping and documentation.  
However, **all engineering decisions** — synchronization choices, data path design, GUI UX, automated testing strategy, and final validation — were made manually.  
This workflow allowed maintaining professional‑grade quality while benefiting from rapid iteration.

---
