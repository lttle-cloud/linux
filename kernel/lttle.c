#include <lttle/trigger.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/printk.h>

static volatile void *mapped_mmio_base = 0;

typedef struct {
    unsigned char code;
    // padding until 8 bytes
    unsigned char padding[7];
} lttle_sys_trigger_data;

typedef char lttle_sys_trigger_data_incomplete_size[sizeof(lttle_sys_trigger_data) == 8 ? 1 : -1]; // Ensure the size of the struct is 8 bytes

int __init lttle_subsystem_init(void)
{
    mapped_mmio_base = ioremap(LTTLE_TRIGGER_MEMORY_BASE, LTTLE_TRIGGER_MEMORY_SIZE);
    if (!mapped_mmio_base) {
        pr_err("Failed to map MMIO region for LTTLE subsystem\n");
        return -1;
    }

    pr_info("LTTLE subsystem initialized, MMIO base mapped at %px\n", mapped_mmio_base);
    return 0;
}

void __exit lttle_subsystem_exit(void)
{
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
