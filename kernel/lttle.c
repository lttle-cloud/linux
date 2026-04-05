#include <lttle/trigger.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/pid_namespace.h>
#include <linux/pid.h>

static volatile void *mapped_mmio_base = 0;

/* File watch state */
static char watched_paths[LTTLE_FILE_WATCH_MAX][256];
static char watched_names[LTTLE_FILE_WATCH_MAX][64];
static bool watch_active[LTTLE_FILE_WATCH_MAX];

static void lttle_emit_event(int file_index)
{
	struct pid *pid;
	int sig = (file_index == 0) ? SIGUSR1 : SIGUSR2;
	int ret;

	/* Send signal to the process (not a specific thread) so that
	 * any thread waiting via sigwait() can pick it up. */
	rcu_read_lock();
	pid = find_pid_ns(1, &init_pid_ns);
	if (pid) {
		struct task_struct *task = pid_task(pid, PIDTYPE_PID);
		pr_info("lttle: emit signal %d to pid 1 (task=%s, pid=%d, tgid=%d)\n",
			sig, task ? task->comm : "NULL",
			task ? task->pid : -1,
			task ? task->tgid : -1);
		ret = kill_pid(pid, sig, 1);
		pr_info("lttle: kill_pid returned %d\n", ret);
	} else {
		pr_warn("lttle: find_pid_ns(1, init_pid_ns) returned NULL!\n");
	}
	rcu_read_unlock();
}

void lttle_set_watch(int index, const char *path)
{
	const char *slash;

	if (index < 0 || index >= LTTLE_FILE_WATCH_MAX)
		return;

	strscpy(watched_paths[index], path, sizeof(watched_paths[index]));
	slash = strrchr(path, '/');
	strscpy(watched_names[index], slash ? slash + 1 : path,
		sizeof(watched_names[index]));
	watch_active[index] = true;
	pr_info("lttle: watching path[%d] = %s (name=%s)\n",
		index, watched_paths[index], watched_names[index]);
}

void lttle_check_close(struct file *file)
{
	int i;
	const char *name;
	char buf[256];
	char *path;

	if (!(file->f_mode & FMODE_WRITE))
		return;

	name = file->f_path.dentry->d_name.name;
	for (i = 0; i < LTTLE_FILE_WATCH_MAX; i++) {
		if (!watch_active[i])
			continue;
		if (strcmp(name, watched_names[i]) != 0)
			continue;
		/* Filename matches, do full path check */
		path = d_path(&file->f_path, buf, sizeof(buf));
		if (IS_ERR(path))
			continue;
		if (strcmp(path, watched_paths[i]) == 0) {
			pr_info("lttle: file close detected [%d] %s\n",
				i, path);
			lttle_emit_event(i);
			break;
		}
	}
}

void lttle_check_rename(const struct path *rpath)
{
	int i;
	const char *name;
	char buf[256];
	char *path;

	name = rpath->dentry->d_name.name;
	for (i = 0; i < LTTLE_FILE_WATCH_MAX; i++) {
		if (!watch_active[i])
			continue;
		if (strcmp(name, watched_names[i]) != 0)
			continue;
		path = d_path(rpath, buf, sizeof(buf));
		if (IS_ERR(path))
			continue;
		if (strcmp(path, watched_paths[i]) == 0) {
			pr_info("lttle: file rename detected [%d] %s\n",
				i, path);
			lttle_emit_event(i);
			break;
		}
	}
}

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

static long lttle_proc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case LTTLE_IOC_WATCH: {
        struct lttle_watch_req req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        req.path[sizeof(req.path) - 1] = '\0';
        if (req.index >= LTTLE_FILE_WATCH_MAX)
            return -EINVAL;
        lttle_set_watch(req.index, req.path);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static const struct proc_ops lttle_proc_ops = {
    .proc_read = lttle_proc_read,
    .proc_write = lttle_proc_write,
    .proc_ioctl = lttle_proc_ioctl,
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
