#ifndef _LTTLE_TRIGGER_H
#define _LTTLE_TRIGGER_H

#include <linux/io.h>
#include <linux/ioctl.h>

struct file;
struct path;

struct lttle_watch_req {
	__u32 index;
	char path[256];
};

#define LTTLE_IOC_MAGIC 'L'
#define LTTLE_IOC_WATCH _IOW(LTTLE_IOC_MAGIC, 1, struct lttle_watch_req)

/* Triggers are sent via PIO port 0x510 (8-byte outl pairs).
 * No MMIO mapping needed. */
#define LTTLE_TRIGGER_PIO_PORT (0x510)

#define LTTLE_SYS_AFTER_OFFSET 127
#define LTTLE_SYS_CMD_OFFSET 64

#define LTTLE_SYS_LISTEN_BEFORE 1
#define LTTLE_SYS_BIND_BEFORE 2
#define LTTLE_USERSPACE_READY 3
#define LTTLE_MANUAL_TRIGGER 10

#define LTTLE_SYS_LISTEN_AFTER (LTTLE_SYS_LISTEN_BEFORE + LTTLE_SYS_AFTER_OFFSET)
#define LTTLE_SYS_BIND_AFTER (LTTLE_SYS_BIND_BEFORE + LTTLE_SYS_AFTER_OFFSET)

#define LTTLE_CMD_FLASH_LOCK (LTTLE_SYS_CMD_OFFSET + 0)
#define LTTLE_CMD_FLASH_UNLOCK (LTTLE_SYS_CMD_OFFSET + 1)

#define LTTLE_FILE_WATCH_MAX 2

int __init lttle_subsystem_init(void);
void __exit lttle_subsystem_exit(void);
void lttle_sys_trigger(unsigned char code, char data[7]);
void lttle_sys_cmd(unsigned char cmd);
void lttle_set_watch(int index, const char *path);
void lttle_check_close(struct file *file);
void lttle_check_rename(const struct path *path);

#endif // _LTTLE_TRIGGER_H
