#include <linux/init.h>     // For macros module_init and module_exit
#include <linux/module.h>   // Core header for loading LKMs into the kernel
#include <linux/kernel.h>   // Contains types, macros, functions for the kernel

// Module information
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Giuseppe Pia");
MODULE_DESCRIPTION("A simple Hello World kernel module");
MODULE_VERSION("0.1");

// Function executed when the module is loaded
static int __init hello_init(void)
{
    printk(KERN_INFO "Hello from kernel space!\n");
    return 0; // 0 = success
}

// Function executed when the module is removed
static void __exit hello_exit(void)
{
    printk(KERN_INFO "Goodbye world\n");
}

// Register entry and exit points
module_init(hello_init);
module_exit(hello_exit);
