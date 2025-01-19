#ifndef _LTTLE_TRIGGER_H
#define _LTTLE_TRIGGER_H

#include <linux/io.h>

#define LTTLE_TRIGGER_MEMORY_BASE (0xd0000000)
#define LTTLE_TRIGGER_MEMORY_SIZE (4096)

#define LTTLE_SYS_AFTER_OFFSET 127

#define LTTLE_SYS_LISTEN_BEFORE 1
#define LTTLE_SYS_SOCK_BEFORE 2
#define LTTLE_SYS_BIND_BEFORE 3

#define LTTLE_SYS_LISTEN_AFTER (LTTLE_SYS_LISTEN_BEFORE + LTTLE_SYS_AFTER_OFFSET)
#define LTTLE_SYS_SOCK_AFTER (LTTLE_SYS_SOCK_BEFORE + LTTLE_SYS_AFTER_OFFSET)
#define LTTLE_SYS_BIND_AFTER (LTTLE_SYS_BIND_BEFORE + LTTLE_SYS_AFTER_OFFSET)

int __init lttle_subsystem_init(void);
void __exit lttle_subsystem_exit(void);
void lttle_sys_trigger(unsigned char code, void* data);

#endif // _LTTLE_TRIGGER_H
