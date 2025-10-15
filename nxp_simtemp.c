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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gius Pianfort");
MODULE_DESCRIPTION("nxp_simtemp: periodic virtual temperature source");
MODULE_VERSION("0.2");

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

/* ---- Producer work: generates one sample and pushes to ring ---- */
static void simtemp_work_fn(struct work_struct *work)
{
    struct simtemp_dev *d = gdev;
    struct simtemp_sample s;
    unsigned long flags;

    /* Generate a synthetic sample: simple sawtooth in mC */
    static s32 ramp = 20000; /* 20.000 °C */
    ramp += 123;             /* +0.123 °C per sample */
    if (ramp > 45000) ramp = 20000;

    s.timestamp_ns = ktime_get_ns();
    s.temp_mC      = ramp;
    s.flags        = 1u;     /* bit0 = NEW_SAMPLE */

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

static const struct file_operations simtemp_fops = {
    .owner  = THIS_MODULE,
    .read   = simtemp_read,
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

    INIT_WORK(&gdev->work, simtemp_work_fn);

    ret = misc_register(&simtemp_miscdev);
    if (ret) {
        pr_err("simtemp: misc_register failed: %d\n", ret);
        kfree(gdev);
        return ret;
    }

    /* timer @ SIMTEMP_PERIOD_MS */
    gdev->period = ktime_set(0, SIMTEMP_PERIOD_MS * 1000000ULL);
    hrtimer_init(&gdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
    gdev->timer.function = simtemp_timer_fn;
    hrtimer_start(&gdev->timer, gdev->period, HRTIMER_MODE_REL_PINNED);

    pr_notice("simtemp: /dev/%s up, period=%d ms, ring=%d\n",
              simtemp_miscdev.name, SIMTEMP_PERIOD_MS, RING_SIZE);
    return 0;
}

static void __exit simtemp_exit(void)
{
    /* stop producer first */
    hrtimer_cancel(&gdev->timer);
    flush_work(&gdev->work);

    misc_deregister(&simtemp_miscdev);

    kfree(gdev);
    pr_notice("simtemp: /dev/%s down\n", simtemp_miscdev.name);
}

module_init(simtemp_init);
module_exit(simtemp_exit);
