#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>   // ktime_get_ns
#include "nxp_simtemp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Giuseppe Pia");
MODULE_DESCRIPTION("nxp_simtemp: skeleton char device");
MODULE_VERSION("0.1");

// For now: return one synthetic sample and EOF afterward
static ssize_t simtemp_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos)
{
    struct simtemp_sample s;
    size_t len = sizeof(s);

    if (*ppos >= len)
        return 0; // EOF after one record (temporary behavior)

    s.timestamp_ns = ktime_get_ns();
    s.temp_mC      = 42123;           // 42.123 °C (placeholder)
    s.flags        = 1u;              // bit0 NEW_SAMPLE

    // allow partial reads
    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(buf, ((u8 *)&s) + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
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
    .mode  = 0666, // facilitar pruebas
};

static int __init simtemp_init(void)
{
    int ret = misc_register(&simtemp_miscdev);
    if (ret) {
        pr_err("simtemp: misc_register failed: %d\n", ret);
        return ret;
    }
    pr_notice("simtemp: /dev/%s registered (skeleton)\n", simtemp_miscdev.name);
    return 0;
}

static void __exit simtemp_exit(void)
{
    misc_deregister(&simtemp_miscdev);
    pr_notice("simtemp: /dev/%s deregistered\n", simtemp_miscdev.name);
}

module_init(simtemp_init);
module_exit(simtemp_exit);
