# DESIGN.md — NXP Simulated Temperature Sensor (`nxp_simtemp`)

**Author:** José Giuseppe Pia Figueroa  
**Date:** October 2025  
**Version:** 1.0  

---

## 1. Architecture Overview

The `nxp_simtemp` project implements a **virtual temperature sensor** using a Linux kernel module that communicates with user space through a **character device** (`/dev/simtemp`).  
The driver simulates temperature samples periodically and provides configuration and event notification mechanisms.

### Main Components

|     Layer      |         Component          |              Description               |
|----------------|----------------------------|----------------------------------------|
| **Kernel**     | `nxp_simtemp.ko`           | Out-of-tree kernel module implementing |
|                |                            |    the simulated sensor                |
| **User-space** | `cli/simtemp_cli.py`       | Python command-line application to     |
|                |                            |    configure and read samples          |
| **Interface**  | `/dev/simtemp`             | Character device for data path         |
|                |                            |    (read/poll)                         |
| **Sysfs**      | `/sys/class/misc/simtemp/` | Attribute files for configuration and  |
|                |                            |    statistics                          |
| **Automation** | `scripts/`                 | Build and demo scripts                 |

---

## 2. Interaction Diagram

```
             ┌────────────────────────────────────────────┐
             │                  User Space                │
             │                                            │
             │  ┌────────────────────────────────────┐    │
             │  │  simtemp_cli.py (poll/read/sysfs)  │    │
             │  └──────────────┬─────────────────────┘    │
             │                 │                          │
             └─────────────────┼──────────────────────────┘
                               │
                               │ Character device (/dev/simtemp)
                               │
             ┌─────────────────▼──────────────────────────┐
             │                Kernel Space                │
             │                                            │
             │  nxp_simtemp.ko                            │
             │  ├─ hrtimer: triggers periodic work        │
             │  ├─ workqueue: generates temperature sample│
             │  ├─ ring buffer: stores samples            │
             │  ├─ wait queue: syncs readers (poll/read)  │
             │  ├─ sysfs attrs: sampling_ms, threshold... │
             │  └─ miscdevice: exposes /dev/simtemp       │
             └────────────────────────────────────────────┘
```

---

## 3. Data Flow

### Sampling and Event Path

1. **Initialization**
   - Driver registers a `miscdevice` → `/dev/simtemp`.
   - Initializes `hrtimer`, `workqueue`, ring buffer, and wait queue.

2. **Periodic Sampling**
   - `hrtimer` triggers every `sampling_ms`.
   - Timer callback schedules a `work` via `schedule_work()`.
   - The work handler (`simtemp_work_fn`) generates a new temperature sample.

3. **Data Storage**
   - Sample pushed into a **ring buffer** under spinlock.
   - Wait queue (`wake_up_interruptible`) notifies any blocked readers.

4. **User-space Read**
   - CLI executes `poll()` → unblocks when data available or threshold crossed.
   - `read()` copies one `simtemp_sample` structure to user space.

5. **Configuration (Control Path)**
   - Sysfs attributes update parameters in `gdev` (protected by mutexes).
   - Example: `echo 50 > sampling_ms` updates timer period.

---

## 4. Kernel Internals

### Core Data Structures

```c
struct simtemp_sample {
    __u64 timestamp_ns;
    __s32 temp_mC;
    __u32 flags;      // bit0: NEW_SAMPLE, bit1: THRESHOLD_CROSSED
} __attribute__((packed));

struct simtemp_dev {
    struct simtemp_sample buf[RING_SIZE];
    u32 head;                   // next write index
    u32 tail;                   // next read index
    spinlock_t lock;            // protects the ring buffer
    wait_queue_head_t wq;       // reader wait queue
    atomic64_t total_samples;   // total samples generated
    atomic64_t threshold_crossings; // count of threshold events
    int period_ms;              // sampling period (ms)
    s32 threshold_mC;           // alert threshold (milli-Celsius)
    int mode;                   // 0=normal, 1=noisy, 2=ramp
    bool above_threshold;       // was previous sample above threshold?
    struct hrtimer timer;       // hrtimer handle
    ktime_t period;             // hrtimer period value
    struct work_struct work;    // workqueue handle
};
```

### Concurrency

- **Spinlock** protects the ring buffer and event flags.
- **Mutex** (if used) guards configuration changes (sysfs writes).
- **Wait queue** synchronizes readers waiting for data.
- **Workqueue** runs in process context to avoid heavy work in timer interrupt.

---

## 5. Locking and Synchronization Choices

| Resource                 | Primitive           | Rationale                                                          |
|--------------------------|---------------------|--------------------------------------------------------------------|
| **Ring buffer**          | `spinlock_t`        | Very short critical section; used in timer/work context.           |
| **Configuration params** | `mutex`             | Safe updates from sysfs; can sleep.                                |
| **Reader wakeups**       | `wait_queue_head_t` | Efficient event signaling for `poll()` and `read()`.               |


---

## 6. Scaling and Limitations

At ~10 kHz sampling, the following bottlenecks appear:

| Area                   | Limitation                          | Mitigation                                          |
|------------------------|-------------------------------------|-----------------------------------------------------|
| Workqueue latency      | Kernel thread scheduling overhead   | Use hrtimer only + direct push for ultra-high rates |
| Ring buffer contention | Spinlock hot path                   | Consider lock-free circular buffer                  |
| User-space I/O         | Context switch overhead             | Batch reads or mmap shared buffer                   |
| Sysfs writes           | Non-real-time                       | Use ioctl() for atomic updates                      |

For this demo (100 ms period), standard mechanisms are perfectly adequate.

---

## 7. Summary of Interaction

| **Path**         | **Mechanism**                           | **Notes**                                |
|------------------|-----------------------------------------|------------------------------------------|
| Data → user      | char device `/dev/simtemp`              | `read()` returns `struct simtemp_sample` |
| Event notify     | `poll()` / `wake_up_interruptible()`    | `POLLPRI` on threshold cross             |
| Control ← user   | sysfs attributes                        | Simple and human-readable                |
| Timing           | `hrtimer` + `workqueue`                 | Accurate, non-blocking producer          |


---

## 8. Diagram — Simplified Timing

```
Time (ms) →

[hrtimer expires] ─▶ [schedule_work()] ─▶ [generate sample] ─▶ [rb_push()] ─▶ [wake_up_interruptible()]
       │                   │                    │                    │
       ▼                   ▼                    ▼                    ▼
     (IRQ)           (Process context)    (spinlock)         (Reader poll/read unblocked)
```

---

## 9. Conclusion

The `nxp_simtemp` module demonstrates a clean, modular kernel-to-user communication path using standard Linux mechanisms:

- **Accurate timing** via high-resolution timers.  
- **Thread-safe buffering** with minimal locking.  
- **Responsive user-space interface** via pollable char device.  
- **Simple control plane** through sysfs.  

This architecture balances clarity, reliability, and extensibility—ideal for showcasing embedded Linux driver design skills.
