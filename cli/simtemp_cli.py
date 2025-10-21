#!/usr/bin/env python3
import argparse, os, struct, time, glob, select, sys

DEV = "/dev/simtemp"
REC = struct.Struct("=Q i I")  # u64 ns, s32 mC, u32 flags (packed)
FLAG_NEW = 1 << 0
FLAG_THRESH = 1 << 1


def write_attr(base, name, value):
    if not base: return
    try:
        with open(os.path.join(base, name), "w") as f:
            f.write(str(value))
    except Exception as e:
        print(f"[warn] sysfs {name}: {e}", file=sys.stderr)

def main():
    ap = argparse.ArgumentParser(description="simtemp CLI (poll + read)")
    ap.add_argument("--sampling-ms", type=int)
    ap.add_argument("--threshold-mC", type=int)
    ap.add_argument("--mode", choices=["normal","noisy","ramp"])
    ap.add_argument("--test", action="store_true", help="trigger threshold within ~2 periods")
    args = ap.parse_args()

    sysfs_base = "/sys/class/misc/simtemp"
    if args.sampling_ms:  write_attr(sysfs_base, "sampling_ms", args.sampling_ms)
    if args.threshold_mC: write_attr(sysfs_base, "threshold_mC", args.threshold_mC)
    if args.mode:         write_attr(sysfs_base, "mode", args.mode)

    fd = os.open(DEV, os.O_RDONLY)  # blocking
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLPRI)

    if args.test and sysfs_base:
        # Lower the threshold to get more crossing ev ents
        write_attr(sysfs_base, "threshold_mC", 26000)

    try:
        while True:
            events = poller.poll(5000)  # ms timeout
            if not events:
                print("[timeout] no data"); continue

            for (_fd, ev) in events:
                if ev & (select.POLLERR | select.POLLHUP):
                    print("[error] device"); sys.exit(2)

                if ev & (select.POLLIN | select.POLLPRI):
                    data = os.read(fd, REC.size)
                    if len(data) != REC.size:
                        print("[short read]"); continue
                    ts_ns, temp_mC, flags = REC.unpack(data)
                    alert = 1 if (flags & FLAG_THRESH) else 0
                    temp_C = temp_mC / 1000.0
                    print(f"{ts_ns} temp={temp_C:.3f}C alert={alert}")

                    if args.test and alert:
                        print("TEST: PASS (threshold event)"); sys.exit(0)

            if args.test:
                # Si tras algunos ciclos no hay alerta, falla.
                # (Ajusta sampling/threshold si fuera necesario.)
                pass
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)

if __name__ == "__main__":
    main()
