#include <lttle/trigger.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static volatile void *mapped_mmio_base = 0;

typedef struct {
    unsigned char code;
    // padding until 8 bytes
    unsigned char padding[7];
} lttle_sys_trigger_data;

typedef char lttle_sys_trigger_data_incomplete_size[sizeof(lttle_sys_trigger_data) == 8 ? 1 : -1]; // Ensure the size of the struct is 8 bytes

static ssize_t lttle_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    unsigned long long boot_time = (*(volatile unsigned long long *)mapped_mmio_base);
    
    char msg[1024];
    int len = snprintf(msg, sizeof(msg), "{\"boot_time_us\": %llu}", boot_time);

    return simple_read_from_buffer(buf, count, ppos, msg, len);
}

static ssize_t lttle_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    pr_info("WARNING: lttle_proc_write is not implemented\n");
    return count;
}

static const struct proc_ops lttle_proc_ops = {
    .proc_read = lttle_proc_read,
    .proc_write = lttle_proc_write,
};

static struct proc_dir_entry *lttle_proc_entry;

int __init lttle_subsystem_init(void)
{
    mapped_mmio_base = ioremap(LTTLE_TRIGGER_MEMORY_BASE, LTTLE_TRIGGER_MEMORY_SIZE);
    if (!mapped_mmio_base) {
        pr_err("Failed to map MMIO region for LTTLE subsystem\n");
        return -1;
    }

    lttle_proc_entry = proc_create("lttle", 0666, NULL, &lttle_proc_ops);
    if (!lttle_proc_entry) {
        pr_err("Failed to create /proc/lttle\n");
        iounmap(mapped_mmio_base);
        mapped_mmio_base = 0;
        return -1;
    }

    pr_info("LTTLE subsystem initialized, MMIO base mapped at %px\n", mapped_mmio_base);
    return 0;
}

void __exit lttle_subsystem_exit(void)
{
    if (lttle_proc_entry) {
        proc_remove(lttle_proc_entry);
        lttle_proc_entry = NULL;
    }
    if (mapped_mmio_base) {
        iounmap(mapped_mmio_base);
        pr_info("LTTLE subsystem MMIO region unmapped\n");
        mapped_mmio_base = 0;
    }

    pr_info("LTTLE subsystem exited cleanly\n");
}

void lttle_sys_trigger(unsigned char code, void* data)
{
    lttle_sys_trigger_data trigger_data;
    trigger_data.code = code;

    if (!mapped_mmio_base) {
        pr_err("MMIO base not mapped. Did you call lttle_sys_trigger_init()?\n");
        return;
    }


    *((volatile lttle_sys_trigger_data *)mapped_mmio_base) = trigger_data;
}

subsys_initcall(lttle_subsystem_init);
module_exit(lttle_subsystem_exit);
