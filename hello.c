#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>   // copy_to_user

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Hello char device using miscdevice");
MODULE_VERSION("0.2");

static const char hello_msg[] = "Hello from /dev/hello\n";

static ssize_t hello_read(struct file *file, char __user *buf,
                          size_t count, loff_t *ppos)
{
    size_t len = sizeof(hello_msg) - 1;   // exclude trailing '\0'

    // Implement EOF after the message is read once:
    if (*ppos >= len)
        return 0;

    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(buf, hello_msg + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct file_operations hello_fops = {
    .owner  = THIS_MODULE,
    .read   = hello_read,
    .llseek = no_llseek,
};

static struct miscdevice hello_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "hello",        // device node will be /dev/hello
    .fops  = &hello_fops,
    .mode  = 0666,           // convenience for testing (optional)
};

static int __init hello_init(void)
{
    int ret = misc_register(&hello_miscdev);
    if (ret) {
        pr_err("hello: misc_register failed: %d\n", ret);
        return ret;
    }
    pr_notice("hello: /dev/%s registered\n", hello_miscdev.name);
    return 0;
}

static void __exit hello_exit(void)
{
    misc_deregister(&hello_miscdev);
    pr_notice("hello: /dev/%s deregistered\n", hello_miscdev.name);
}

module_init(hello_init);
module_exit(hello_exit);