#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>   // copy_to_user, copy_from_user

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Hello char device using miscdevice");
MODULE_VERSION("0.3");

static const char hello_msg[] = "Hello from /dev/hello\n";

// Buffer to store written data
#define HELLO_BUFFER_SIZE 1024
static char hello_buffer[HELLO_BUFFER_SIZE];
static size_t hello_buffer_len = 0;

static ssize_t hello_write(struct file *file, const char __user *buf,
                           size_t count, loff_t *ppos)
{
    // Don't allow writing beyond buffer size
    if (count > HELLO_BUFFER_SIZE)
        count = HELLO_BUFFER_SIZE;

    // Copy data from user space to our buffer
    if (copy_from_user(hello_buffer, buf, count))
        return -EFAULT;

    // Update buffer length
    hello_buffer_len = count;
    
    // Reset read position when new data is written
    *ppos = 0;
    
    pr_info("hello: wrote %zu bytes to buffer\n", count);
    return count;
}

static ssize_t hello_read(struct file *file, char __user *buf,
                          size_t count, loff_t *ppos)
{
    size_t len;
    const char *data;

    // If we have written data, use that; otherwise use default message
    if (hello_buffer_len > 0) {
        len = hello_buffer_len;
        data = hello_buffer;
    } else {
        len = sizeof(hello_msg) - 1;   // exclude trailing '\0'
        data = hello_msg;
    }

    // Implement EOF after the message is read once:
    if (*ppos >= len)
        return 0;

    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(buf, data + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct file_operations hello_fops = {
    .owner  = THIS_MODULE,
    .read   = hello_read,
    .write  = hello_write,
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