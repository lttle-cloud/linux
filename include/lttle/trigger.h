#ifndef _LTTLE_TRIGGER_H
#define _LTTLE_TRIGGER_H

#include <linux/io.h>

#define LTTLE_TRIGGER_MEMORY_BASE (0xd0000000)
#define LTTLE_TRIGGER_MEMORY_SIZE (4096)

#define LTTLE_SYS_WRITE 1
#define LTTLE_SYS_LISTEN 2

int __init lttle_subsystem_init(void);
void __exit lttle_subsystem_exit(void);
void lttle_sys_trigger(unsigned char code, void* data);

#endif // _LTTLE_TRIGGER_H
