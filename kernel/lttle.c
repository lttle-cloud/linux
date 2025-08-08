#include <lttle/trigger.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>

static volatile void *mapped_mmio_base = 0;

typedef struct {
    unsigned char code;
    // 7 bytes data
    unsigned char data[7];
} lttle_sys_trigger_data;

typedef char lttle_sys_trigger_data_incomplete_size[sizeof(lttle_sys_trigger_data) == 8 ? 1 : -1]; // Ensure the size of the struct is 8 bytes

static ssize_t lttle_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    unsigned long long last_boot_time = (*(volatile unsigned long long *)mapped_mmio_base);
    unsigned long long first_boot_time = (*(volatile unsigned long long *)(mapped_mmio_base + 8));
    char msg[1024];
    int len = snprintf(msg, sizeof(msg), "{\"last_boot_time_us\": %llu, \"first_boot_time_us\": %llu}", last_boot_time, first_boot_time);

    return simple_read_from_buffer(buf, count, ppos, msg, len);
}

static ssize_t lttle_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    int len;
    char tbuf[256] = {0};

    if (count >= sizeof(tbuf)) {
        return -ENOSPC;
    }

    len = simple_write_to_buffer(tbuf, sizeof(tbuf) - 1, ppos, buf, count);
    if (len < 0) {
        return len;
    }

    tbuf[len] = '\0';

    if (strcmp(tbuf, "manual_trigger") == 0) {
        lttle_sys_trigger(LTTLE_MANUAL_TRIGGER, NULL);
        *ppos = 0;  // Reset position for next write
        return count;
    }

    if (strcmp(tbuf, "flash_lock") == 0) {
        lttle_sys_cmd(LTTLE_CMD_FLASH_LOCK);
        *ppos = 0;  // Reset position for next write
        return count;
    }

    if (strcmp(tbuf, "flash_unlock") == 0) {
        lttle_sys_cmd(LTTLE_CMD_FLASH_UNLOCK);
        *ppos = 0;  // Reset position for next write
        return count;
    }

    *ppos = 0;  // Reset position for next write
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

void lttle_sys_trigger(unsigned char code, char data[7])
{
    lttle_sys_trigger_data trigger_data;
    trigger_data.code = code;
    if (data) {
        memcpy(trigger_data.data, data, sizeof(trigger_data.data));
    }

    if (!mapped_mmio_base) {
        pr_err("MMIO base not mapped. Did you call lttle_sys_trigger_init()?\n");
        return;
    }


    *((volatile lttle_sys_trigger_data *)mapped_mmio_base) = trigger_data;
}

void lttle_sys_cmd(unsigned char cmd)
{
    *((volatile unsigned char *)mapped_mmio_base + 8) = cmd;
}

subsys_initcall(lttle_subsystem_init);
module_exit(lttle_subsystem_exit);
