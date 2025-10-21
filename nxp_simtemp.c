// nxp_simtemp.c
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include "nxp_simtemp.h"
#include <linux/poll.h>
#include <linux/device.h>   /* for sysfs device attributes */
#include <linux/sysfs.h>    /* for DEVICE_ATTR macros */


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gius Pianfort");
MODULE_DESCRIPTION("nxp_simtemp: periodic virtual temperature source");
MODULE_VERSION("1.0");

/* ---- Config (temporal) ---- */
#define SIMTEMP_PERIOD_MS   100       /* 10 Hz */
#define RING_SIZE           128
#define RB_MASK   (RING_SIZE - 1)     /* Power of 2 to use AND instead of % */

/* ---- Device state ---- */
struct simtemp_dev {
    /* ring buffer */
    struct simtemp_sample buf[RING_SIZE];
    u32 head;              /* next write index */
    u32 tail;              /* next read index  */
    spinlock_t lock;

    /* sync & stats */
    wait_queue_head_t wq;  /* readers sleep here when empty */
    atomic64_t total_samples;
    atomic64_t threshold_crossings;
    
    /* configurable parameters */
    int period_ms;         /* sampling period in milliseconds */
    s32 threshold_mC;      /* alert threshold in milli-Celsius */
    int mode;              /* 0=normal, 1=noisy, 2=ramp */
    
    /* threshold crossing detection */
    bool above_threshold;  /* previous sample was above threshold */

    /* producer */
    struct hrtimer timer;
    ktime_t period;
    struct work_struct work;
};

static struct simtemp_dev *gdev;

/* ---- Ring buffer helpers (single-producer, multi-consumer safe with spinlock) ---- */
static inline bool rb_is_empty(struct simtemp_dev *d)
{
    return d->head == d->tail;
}

static inline bool rb_is_full(struct simtemp_dev *d)
{
    return ((d->head + 1) % RING_SIZE) == d->tail;
}

static inline void rb_push(struct simtemp_dev *d, const struct simtemp_sample *s)
{
    if (rb_is_full(d)) {
        d->tail = (d->tail + 1) % RING_SIZE;
    }

    d->buf[d->head] = *s;
    d->head = (d->head + 1) & RB_MASK;
}

static inline bool rb_pop(struct simtemp_dev *d, struct simtemp_sample *out)
{
    if (rb_is_empty(d))
        return false;
    *out = d->buf[d->tail];
    d->tail = (d->tail + 1) % RING_SIZE;
    return true;
}

/* ---- Sysfs attribute handlers ---- */

/* sampling_ms: configurable sampling period in milliseconds */
static ssize_t sampling_ms_show(struct device *dev, 
                                struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", gdev->period_ms);
}

static ssize_t sampling_ms_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    int ms;
    
    /* validate input: must be between 1ms and 10 seconds */
    if (kstrtoint(buf, 10, &ms) || ms < 1 || ms > 10000)
        return -EINVAL;
    
    /* update period and restart timer */
    gdev->period_ms = ms;
    gdev->period = ms_to_ktime(ms);
    
    /* restart timer with new period */
    hrtimer_cancel(&gdev->timer);
    hrtimer_start(&gdev->timer, gdev->period, HRTIMER_MODE_REL_PINNED);
    
    return count;
}

/* threshold_mC: alert threshold in milli-Celsius */
static ssize_t threshold_mC_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", gdev->threshold_mC);
}

static ssize_t threshold_mC_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
    s32 mC;
    
    /* validate input: reasonable temperature range */
    if (kstrtos32(buf, 10, &mC) || mC < -50000 || mC > 150000)
        return -EINVAL;
    
    gdev->threshold_mC = mC;
    return count;
}

/* mode: temperature generation mode */
static ssize_t mode_show(struct device *dev,
                         struct device_attribute *attr, char *buf)
{
    const char *mode_str[] = {"normal", "noisy", "ramp"};
    
    if (gdev->mode >= 0 && gdev->mode < 3)
        return sprintf(buf, "%s\n", mode_str[gdev->mode]);
    else
        return sprintf(buf, "unknown\n");
}

static ssize_t mode_store(struct device *dev,
                          struct device_attribute *attr,
                          const char *buf, size_t count)
{
    int mode;
    
    /* parse mode string - remove trailing newline if present */
    size_t len = count;
    if (len > 0 && buf[len-1] == '\n')
        len--;  /* remove newline for comparison */
    
    /* parse mode string */
    if (strncmp(buf, "normal", 6) == 0 && len == 6)
        mode = 0;
    else if (strncmp(buf, "noisy", 5) == 0 && len == 5)
        mode = 1;
    else if (strncmp(buf, "ramp", 4) == 0 && len == 4)
        mode = 2;
    else
        return -EINVAL;
    
    gdev->mode = mode;
    return count;
}

/* stats: read-only statistics */
static ssize_t stats_show(struct device *dev,
                          struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "total_samples=%lld\nthreshold_crossings=%lld\n", 
                   atomic64_read(&gdev->total_samples),
                   atomic64_read(&gdev->threshold_crossings));
}

/* ---- Device attribute declarations ---- */
static DEVICE_ATTR_RW(sampling_ms);    /* read-write attribute */
static DEVICE_ATTR_RW(threshold_mC);   /* read-write attribute */
static DEVICE_ATTR_RW(mode);           /* read-write attribute */
static DEVICE_ATTR_RO(stats);          /* read-only attribute */

/* ---- Producer work: generates one sample and pushes to ring ---- */
static void simtemp_work_fn(struct work_struct *work)
{
    struct simtemp_dev *d = gdev;
    struct simtemp_sample s;
    unsigned long flags;

    /* Generate temperature based on configured mode */
    s.timestamp_ns = ktime_get_ns();
    s.flags        = 1u;     /* bit0 = NEW_SAMPLE */
    
    switch (d->mode) {
    case 0: /* normal mode: constant temperature */
        s.temp_mC = 25000;  /* 25°C */
        break;
        
    case 1: /* noisy mode: random temperature */
        s.temp_mC = 25000 + (get_random_u32() % 10000) - 5000;  /* 20-30°C */
        break;
        
    case 2: /* ramp mode: sawtooth pattern */
        static s32 ramp = 20000; /* 20.000 °C */
        ramp += 123;             /* +0.123 °C per sample */
        if (ramp > 45000) ramp = 20000;
        s.temp_mC = ramp;
        break;
        
    default:
        s.temp_mC = 25000;  /* fallback to normal */
        break;
    }
    
    /* detect threshold crossing (not just being above threshold) */
    bool currently_above = (s.temp_mC > d->threshold_mC);
    
    if (currently_above != d->above_threshold) {
        /* threshold crossed - set flag and update state */
        s.flags |= 2u;  /* bit1 = THRESHOLD_CROSSED */
        d->above_threshold = currently_above;
        atomic64_inc(&d->threshold_crossings);
        
        pr_info("simtemp: threshold crossed %s (temp=%d mC, threshold=%d mC)\n",
                currently_above ? "UP" : "DOWN", 
                s.temp_mC, d->threshold_mC);
    }

    spin_lock_irqsave(&d->lock, flags);
    rb_push(d, &s);
    spin_unlock_irqrestore(&d->lock, flags);

    atomic64_inc(&d->total_samples);

    /* Wake up any reader waiting for data */
    wake_up_interruptible(&d->wq);
}

/* ---- hrtimer: schedules the work periodically ---- */
static enum hrtimer_restart simtemp_timer_fn(struct hrtimer *t)
{
    struct simtemp_dev *d = container_of(t, struct simtemp_dev, timer);

    /* Schedule work in process context (keep timer handler minimal) */
    schedule_work(&d->work);

    /* rearm */
    hrtimer_forward_now(&d->timer, d->period);
    return HRTIMER_RESTART;
}

/* ---- file operations ---- */
static ssize_t simtemp_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos)
{
    struct simtemp_dev *d = gdev;
    struct simtemp_sample s;
    unsigned long flags;

    /* we deliver whole records only for now */
    if (count < sizeof(s))
        return -EINVAL;

    /* Fast path: try pop; if empty, block unless O_NONBLOCK */
    for (;;) {
        bool ok;

        spin_lock_irqsave(&d->lock, flags);
        ok = rb_pop(d, &s);
        spin_unlock_irqrestore(&d->lock, flags);

        if (ok)
            break;

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        /* sleep until producer wakes us; handle signals */
        if (wait_event_interruptible(d->wq, !rb_is_empty(d)))
            return -ERESTARTSYS;
    }

    if (copy_to_user(buf, &s, sizeof(s)))
        return -EFAULT;

    return sizeof(s);
}

static __poll_t simtemp_poll(struct file *file, poll_table *wait)
{
    struct simtemp_dev *d = gdev;
    __poll_t mask = 0;
    unsigned long flags;

    /* register interest in the wait queue: if no data, will sleep here */
    poll_wait(file, &d->wq, wait);

    spin_lock_irqsave(&d->lock, flags);
    if (!rb_is_empty(d))
        mask |= POLLIN | POLLRDNORM;   // new data available for read
    spin_unlock_irqrestore(&d->lock, flags);

    return mask;
}

static const struct file_operations simtemp_fops = {
    .owner  = THIS_MODULE,
    .read   = simtemp_read,
    .poll   = simtemp_poll,
    .llseek = no_llseek,
};

static struct miscdevice simtemp_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "simtemp",
    .fops  = &simtemp_fops,
    .mode  = 0666,
};

/* ---- Module init/exit ---- */
static int __init simtemp_init(void)
{
    int ret;

    gdev = kzalloc(sizeof(*gdev), GFP_KERNEL);
    if (!gdev)
        return -ENOMEM;

    spin_lock_init(&gdev->lock);
    init_waitqueue_head(&gdev->wq);
    atomic64_set(&gdev->total_samples, 0);
    atomic64_set(&gdev->threshold_crossings, 0);
    
    /* initialize configurable parameters */
    gdev->period_ms = SIMTEMP_PERIOD_MS;
    gdev->threshold_mC = 45000;  /* 45°C default threshold */
    gdev->mode = 2;               /* ramp mode by default */
    gdev->above_threshold = false; /* start below threshold */

    INIT_WORK(&gdev->work, simtemp_work_fn);

    ret = misc_register(&simtemp_miscdev);
    if (ret) {
        pr_err("simtemp: misc_register failed: %d\n", ret);
        kfree(gdev);
        return ret;
    }

    /* create sysfs attributes */
    ret = device_create_file(simtemp_miscdev.this_device, &dev_attr_sampling_ms);
    if (ret) goto err_sysfs;
    
    ret = device_create_file(simtemp_miscdev.this_device, &dev_attr_threshold_mC);
    if (ret) goto err_sysfs;
    
    ret = device_create_file(simtemp_miscdev.this_device, &dev_attr_mode);
    if (ret) goto err_sysfs;
    
    ret = device_create_file(simtemp_miscdev.this_device, &dev_attr_stats);
    if (ret) goto err_sysfs;

    /* timer @ SIMTEMP_PERIOD_MS */
    gdev->period = ktime_set(0, SIMTEMP_PERIOD_MS * 1000000ULL);
    hrtimer_init(&gdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
    gdev->timer.function = simtemp_timer_fn;
    hrtimer_start(&gdev->timer, gdev->period, HRTIMER_MODE_REL_PINNED);

    pr_notice("simtemp: /dev/%s up, period=%d ms, ring=%d\n",
              simtemp_miscdev.name, SIMTEMP_PERIOD_MS, RING_SIZE);
    return 0;

err_sysfs:
    pr_err("simtemp: failed to create sysfs attributes: %d\n", ret);
    misc_deregister(&simtemp_miscdev);
    kfree(gdev);
    return ret;
}

static void __exit simtemp_exit(void)
{
    /* stop producer first */
    hrtimer_cancel(&gdev->timer);
    flush_work(&gdev->work);

    /* remove sysfs attributes */
    device_remove_file(simtemp_miscdev.this_device, &dev_attr_sampling_ms);
    device_remove_file(simtemp_miscdev.this_device, &dev_attr_threshold_mC);
    device_remove_file(simtemp_miscdev.this_device, &dev_attr_mode);
    device_remove_file(simtemp_miscdev.this_device, &dev_attr_stats);

    misc_deregister(&simtemp_miscdev);

    kfree(gdev);
    pr_notice("simtemp: /dev/%s down\n", simtemp_miscdev.name);
}

module_init(simtemp_init);
module_exit(simtemp_exit);
