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
#include <asm/nmi.h>
#include <asm/apic.h>
#include <asm/msr.h>
#include <linux/irq_work.h>
#include <linux/percpu.h>

/* No MMIO mapping needed — triggers go via PIO port 0x510. */

/*
 * Fork resume via NMI — solves the AMD KVM_SET_LAPIC hrtimer bug.
 *
 * On AMD, KVM_SET_LAPIC restores LAPIC register values but doesn't arm
 * the internal hrtimer. The timer only starts when the guest writes to
 * APIC_TMICT (initial count register). After fork restore, all vCPUs are
 * halted and the timer is dead.
 *
 * The VMM writes FORK_PHASE_REQ_RESUME_FORK (3) to guest physical address
 * 0x6348 (fork mailbox + 0x348) and injects an NMI before starting vCPUs.
 * This NMI handler checks the mailbox, and if a fork resume is requested:
 *   1. Writes to APIC_TMICT to trigger KVM to create an hrtimer
 *   2. Reconfigures network, hostname, entropy from fork mailbox
 *
 * The fork mailbox physical address and phase offset must match the VMM.
 */
#define FORK_MAILBOX_PHYS   0x5000
#define FORK_PHASE_OFFSET   0x348
#define FORK_PHASE_REQ_RESUME 3

static volatile void *fork_mailbox_page;

/* Workqueue handler: signal takeoff (PID 1) to do fork reconfiguration.
 * Now that drain_io() is called before snapshot, disk I/O works after
 * restore, so takeoff can read /dev/mem and run ip commands normally. */
static void fork_reconfig_work_fn(struct work_struct *work)
{
	struct pid *pid;
	int ret = -1;
	const char *magic;

	if (!fork_mailbox_page)
		return;

	/* Verify the fork mailbox has "BOXDFORK" magic at offset 0 */
	magic = (const char *)fork_mailbox_page;
	if (memcmp(magic, "BOXDFORK", 8) != 0) {
		pr_warn("lttle: fork mailbox magic mismatch, skipping signal\n");
		return;
	}

	pr_info("lttle: fork detected — ip=%s gw=%s name=%s\n",
		(const char *)fork_mailbox_page + 0x48,
		(const char *)fork_mailbox_page + 0xC8,
		(const char *)fork_mailbox_page + 0x248);

	/* Send signal 42 (SIGRTMIN+10) to PID 1 (takeoff) */
	rcu_read_lock();
	pid = find_pid_ns(1, &init_pid_ns);
	if (pid)
		ret = kill_pid(pid, 42, 1);
	rcu_read_unlock();
	pr_info("lttle: kill_pid(1, 42) = %d\n", ret);
}

static DECLARE_WORK(fork_reconfig_work, fork_reconfig_work_fn);

/* irq_work runs from normal interrupt context, not NMI context.
 * This is where we re-arm the LAPIC timer and re-init kvmclock. */

static void fork_resume_irq_work(struct irq_work *work)
{
	u32 lvt, lvt_new, tmict, apicbase_lo, apicbase_hi;
	u64 msr_val;
	int cpu = smp_processor_id();

	/*
	 * Step 1: Re-initialize kvmclock.
	 *
	 * After fork, the pvclock shared page has stale parameters from the
	 * source VM. The guest computes timer periods using these stale values,
	 * producing absurd results (e.g. 992ns periods). Re-writing
	 * MSR_KVM_SYSTEM_TIME_NEW with its current value forces KVM to
	 * recompute the pvclock parameters for this VM's TSC.
	 */
	rdmsrl(0x4b564d01, msr_val); /* MSR_KVM_SYSTEM_TIME_NEW */
	if (msr_val) {
		pr_info("lttle: CPU %d re-init kvmclock: MSR_KVM_SYSTEM_TIME_NEW=0x%llx\n",
			cpu, msr_val);
		wrmsrl(0x4b564d01, msr_val);
	}

	/*
	 * Step 2: Re-arm LAPIC timer.
	 *
	 * On AMD, KVM_SET_LAPIC restores register values but doesn't arm
	 * the host hrtimer. Writing APIC_TMICT triggers a VM exit that
	 * creates the hrtimer. Use periodic mode at ~10Hz.
	 */
	lvt = apic_read(APIC_LVTT);
	tmict = apic_read(APIC_TMICT);
	rdmsr(MSR_IA32_APICBASE, apicbase_lo, apicbase_hi);

	pr_info("lttle: CPU %d BEFORE: LVTT=0x%08x TMICT=%u APICBASE=0x%08x\n",
		cpu, lvt, tmict, apicbase_lo);

	lvt_new = (lvt & 0xFF) | (1 << 17); /* periodic + keep vector */
	apic_write(APIC_LVTT, lvt_new);
	apic_write(APIC_TMICT, 23700000);

	pr_info("lttle: CPU %d AFTER: LVTT=0x%08x TMICT=%u TMCCT=%u\n",
		cpu, apic_read(APIC_LVTT), apic_read(APIC_TMICT),
		apic_read(APIC_TMCCT));

	/* Step 3: Schedule fork reconfiguration (network, hostname, entropy).
	 * Only CPU 0 does this. call_usermodehelper needs process context,
	 * so we schedule it on a workqueue. */
	if (cpu == 0 && fork_mailbox_page) {
		/* Clear the fork phase so subsequent NMIs are ignored */
		*(volatile u8 *)((char *)fork_mailbox_page + FORK_PHASE_OFFSET) = 0;
		schedule_work(&fork_reconfig_work);
	}
}

static DEFINE_PER_CPU(struct irq_work, fork_resume_work) =
	IRQ_WORK_INIT(fork_resume_irq_work);

static int lttle_nmi_handler(unsigned int type, struct pt_regs *regs)
{
	u8 phase;

	if (!fork_mailbox_page)
		return NMI_DONE;

	phase = *(volatile u8 *)((char *)fork_mailbox_page + FORK_PHASE_OFFSET);
	if (phase != FORK_PHASE_REQ_RESUME)
		return NMI_DONE;

	pr_info("lttle: NMI handler CPU %d: phase=%d LVTT=0x%08x TMICT=%u TMCCT=%u\n",
		smp_processor_id(), phase,
		apic_read(APIC_LVTT), apic_read(APIC_TMICT), apic_read(APIC_TMCCT));

	/* Schedule the actual timer re-arm from normal context.
	 * KVM on AMD may not arm the hrtimer from NMI context.
	 * Use per-CPU irq_work so every vCPU re-inits its own timers.
	 * Don't clear the phase here — both CPUs need to see it.
	 * The irq_work handler on CPU 0 will clear it after signaling takeoff. */
	irq_work_queue(this_cpu_ptr(&fork_resume_work));

	return NMI_HANDLED;
}

/* File watch state */
static char watched_paths[LTTLE_FILE_WATCH_MAX][256];
static char watched_names[LTTLE_FILE_WATCH_MAX][64];
static bool watch_active[LTTLE_FILE_WATCH_MAX];

static void lttle_emit_event(int file_index)
{
	struct pid *pid;
	int sig = (file_index == 0) ? SIGUSR1 : (file_index == 1) ? SIGUSR2 : (SIGRTMIN + file_index);
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
    /* Boot time counters are no longer available via MMIO. Return 0. */
    unsigned long long last_boot_time = 0;
    unsigned long long first_boot_time = 0;
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
    lttle_proc_entry = proc_create("lttle", 0666, NULL, &lttle_proc_ops);
    if (!lttle_proc_entry) {
        pr_err("Failed to create /proc/lttle\n");
        return -1;
    }

    /* Map the fork mailbox page for the NMI handler */
    fork_mailbox_page = ioremap(FORK_MAILBOX_PHYS, PAGE_SIZE);
    if (!fork_mailbox_page)
        pr_warn("LTTLE: failed to map fork mailbox at 0x%x\n", FORK_MAILBOX_PHYS);

    /* Register NMI handler for fork resume */
    register_nmi_handler(NMI_UNKNOWN, lttle_nmi_handler, 0, "lttle_fork");

    pr_info("LTTLE subsystem initialized (PIO triggers on port 0x%x)\n", LTTLE_TRIGGER_PIO_PORT);
    return 0;
}

void __exit lttle_subsystem_exit(void)
{
    unregister_nmi_handler(NMI_UNKNOWN, "lttle_fork");

    if (lttle_proc_entry) {
        proc_remove(lttle_proc_entry);
        lttle_proc_entry = NULL;
    }
    if (fork_mailbox_page) {
        iounmap(fork_mailbox_page);
        fork_mailbox_page = NULL;
    }

    pr_info("LTTLE subsystem exited cleanly\n");
}

void lttle_sys_trigger(unsigned char code, char data[7])
{
    lttle_sys_trigger_data trigger_data;
    trigger_data.code = code;
    if (data) {
        memcpy(trigger_data.data, data, sizeof(trigger_data.data));
    } else {
        memset(trigger_data.data, 0, sizeof(trigger_data.data));
    }

    /* Send 8-byte trigger as two 4-byte PIO writes to port 0x510 and 0x514.
     * The VMM reconstructs the 8 bytes from these two writes. */
    outl(*(u32 *)&trigger_data, LTTLE_TRIGGER_PIO_PORT);
    outl(*((u32 *)&trigger_data + 1), LTTLE_TRIGGER_PIO_PORT + 4);
}

void lttle_sys_cmd(unsigned char cmd)
{
    /* Commands use port 0x518 (single byte) */
    outb(cmd, LTTLE_TRIGGER_PIO_PORT + 8);
}

subsys_initcall(lttle_subsystem_init);
module_exit(lttle_subsystem_exit);
