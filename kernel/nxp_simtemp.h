#pragma once
#include <linux/types.h>

struct simtemp_sample {
    __u64 timestamp_ns;  // monotonic timestamp
    __s32 temp_mC;       // e.g., 44123 = 44.123 °C
    __u32 flags;         // bit0=NEW_SAMPLE, bit1=THRESHOLD_CROSSED
} __attribute__((packed));
