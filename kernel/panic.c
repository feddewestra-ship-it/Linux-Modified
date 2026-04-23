// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/panic.c — Kernel panic, oops, taint, and warning infrastructure
 *
 * Copyright (C) 1991, 1992  Linus Torvalds
 *
 * This file implements the kernel's last-resort error handling path.
 * It is used throughout the kernel (mm, fs, drivers, …) whenever a
 * situation arises from which recovery is impossible.
 *
 * Key entry points:
 *   panic()         — non-returning: print message, stop CPUs, optionally reboot
 *   vpanic()        — va_list variant of panic()
 *   nmi_panic()     — NMI-safe wrapper around panic()
 *   oops_enter()    — called by arch oops handlers before printing
 *   oops_exit()     — called by arch oops handlers after printing
 *   __warn()        — core implementation of WARN*()
 *   add_taint()     — mark the kernel as tainted
 */

#include <linux/debug_locks.h>
#include <linux/sched/debug.h>
#include <linux/interrupt.h>
#include <linux/kgdb.h>
#include <linux/kmsg_dump.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/vt_kern.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/ftrace.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/kexec.h>
#include <linux/panic_notifier.h>
#include <linux/sched.h>
#include <linux/string_helpers.h>
#include <linux/sysrq.h>
#include <linux/init.h>
#include <linux/nmi.h>
#include <linux/console.h>
#include <linux/bug.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <linux/sysfs.h>
#include <linux/context_tracking.h>
#include <linux/seq_buf.h>
#include <linux/sys_info.h>
#include <trace/events/error_report.h>
#include <asm/sections.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal constants
 * ═════════════════════════════════════════════════════════════════════════*/

/* Panic countdown timer: step size in milliseconds. */
#define PANIC_TIMER_STEP	100

/* Panic LED blink speed: blinks per hour. */
#define PANIC_BLINK_SPD		18

/* Size of the formatted panic message buffer. */
#define PANIC_MSG_BUFSZ		1024

/* ═══════════════════════════════════════════════════════════════════════════
 * Module-level variables
 * ═════════════════════════════════════════════════════════════════════════*/

#ifdef CONFIG_SMP
/*
 * Dump all CPU backtraces on oops.  Defaults to 0; adjustable via sysctl
 * kernel.oops_all_cpu_backtrace.
 */
static unsigned int __read_mostly sysctl_oops_all_cpu_backtrace;
#else
# define sysctl_oops_all_cpu_backtrace 0
#endif /* CONFIG_SMP */

int panic_on_oops = IS_ENABLED(CONFIG_PANIC_ON_OOPS);

/*
 * tainted_mask: bitmask of active TAINT_* flags.
 * Pre-seeded with TAINT_RANDSTRUCT when CONFIG_RANDSTRUCT is enabled,
 * because the build-time struct randomisation is already applied.
 */
static unsigned long tainted_mask =
	IS_ENABLED(CONFIG_RANDSTRUCT) ? (1 << TAINT_RANDSTRUCT) : 0;

/*
 * pause_on_oops: when non-zero, the oops handler serialises concurrent
 * oopses, giving each CPU a chance to print without interleaving.
 * Set via the "pause_on_oops=<seconds>" kernel parameter.
 */
static int pause_on_oops;
static int pause_on_oops_flag;
static DEFINE_SPINLOCK(pause_on_oops_lock);

bool  crash_kexec_post_notifiers;
int   panic_on_warn __read_mostly;
unsigned long panic_on_taint;
bool  panic_on_taint_nousertaint;

/* warn_limit: maximum number of WARN() events before triggering panic. */
static unsigned int warn_limit __read_mostly;

/* panic_console_replay: replay full console log on panic. */
static bool panic_console_replay;

/* Set while gathering all-CPU backtraces during panic. */
bool panic_triggering_all_cpu_backtrace;

/* True once the panicking CPU has printed its own backtrace. */
static bool panic_this_cpu_backtrace_printed;

int panic_timeout = CONFIG_PANIC_TIMEOUT;
EXPORT_SYMBOL_GPL(panic_timeout);

/* panic_print / panic_sys_info: bitmask of extra info to print on panic. */
unsigned long panic_print;

/*
 * panic_force_cpu: if >= 0, redirect panic handling to this CPU.
 * Set via "panic_force_cpu=<n>" early boot parameter.
 * Only meaningful when CONFIG_SMP && CONFIG_CRASH_DUMP.
 */
static int panic_force_cpu = -1;

/* Global panic notifier chain — registered handlers run during panic(). */
ATOMIC_NOTIFIER_HEAD(panic_notifier_list);
EXPORT_SYMBOL(panic_notifier_list);

/* ═══════════════════════════════════════════════════════════════════════════
 * panic_print deprecation
 * ═════════════════════════════════════════════════════════════════════════*/

static void panic_print_deprecated(void)
{
	pr_info_once(
		"Kernel: The 'panic_print' parameter is deprecated. "
		"Use 'panic_sys_info' and 'panic_console_replay' instead.\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * sysctl handlers  (CONFIG_SYSCTL)
 * ═════════════════════════════════════════════════════════════════════════*/

#ifdef CONFIG_SYSCTL

/*
 * proc_taint — sysctl handler for /proc/sys/kernel/tainted.
 *
 * Taint values can only be increased (never cleared via sysctl) to prevent
 * a compromised userspace from hiding kernel taints.  The handler uses a
 * temporary local copy and applies changes atomically via add_taint().
 */
static int proc_taint(const struct ctl_table *table, int write,
		      void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	unsigned long tmptaint = get_taint();
	int err;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &tmptaint;
	err = proc_doulongvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

	if (write) {
		int i;

		/*
		 * If panic_on_taint_nousertaint is set, reject any userspace
		 * write that would trigger a panic via panic_on_taint, to
		 * avoid false positives from compromised userspace.
		 */
		if (panic_on_taint_nousertaint && (tmptaint & panic_on_taint))
			return -EINVAL;

		/*
		 * Apply each requested taint flag individually via add_taint().
		 * This avoids needing a dedicated atomic-or primitive.
		 */
		for (i = 0; i < TAINT_FLAGS_COUNT; i++)
			if ((1UL << i) & tmptaint)
				add_taint(i, LOCKDEP_STILL_OK);
	}

	return err;
}

static int sysctl_panic_print_handler(const struct ctl_table *table, int write,
				      void *buffer, size_t *lenp, loff_t *ppos)
{
	if (write)
		panic_print_deprecated();
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}

static const struct ctl_table kern_panic_table[] = {
#ifdef CONFIG_SMP
	{
		.procname	= "oops_all_cpu_backtrace",
		.data		= &sysctl_oops_all_cpu_backtrace,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
#endif
	{
		.procname	= "tainted",
		.maxlen		= sizeof(long),
		.mode		= 0644,
		.proc_handler	= proc_taint,
	},
	{
		.procname	= "panic",
		.data		= &panic_timeout,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "panic_on_oops",
		.data		= &panic_on_oops,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "panic_print",
		.data		= &panic_print,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= sysctl_panic_print_handler,
	},
	{
		.procname	= "panic_on_warn",
		.data		= &panic_on_warn,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "warn_limit",
		.data		= &warn_limit,
		.maxlen		= sizeof(warn_limit),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
#if (defined(CONFIG_X86_32) || defined(CONFIG_PARISC)) && \
	defined(CONFIG_DEBUG_STACKOVERFLOW)
	{
		.procname	= "panic_on_stackoverflow",
		.data		= &sysctl_panic_on_stackoverflow,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
#endif
	{
		.procname	= "panic_sys_info",
		.data		= &panic_print,
		.maxlen		= sizeof(panic_print),
		.mode		= 0644,
		.proc_handler	= sysctl_sys_info_handler,
	},
};

static __init int kernel_panic_sysctls_init(void)
{
	register_sysctl_init("kernel", kern_panic_table);
	return 0;
}
late_initcall(kernel_panic_sysctls_init);

#endif /* CONFIG_SYSCTL */

/* ═══════════════════════════════════════════════════════════════════════════
 * Boot parameters
 * ═════════════════════════════════════════════════════════════════════════*/

/* panic_sys_info=tasks,mem,locks,ftrace,… */
static int __init setup_panic_sys_info(char *buf)
{
	/* No races during single-threaded early boot. */
	panic_print = sys_info_parse_param(buf);
	return 1;
}
__setup("panic_sys_info=", setup_panic_sys_info);

/* ═══════════════════════════════════════════════════════════════════════════
 * WARN counter and sysfs export
 * ═════════════════════════════════════════════════════════════════════════*/

static atomic_t warn_count = ATOMIC_INIT(0);

#ifdef CONFIG_SYSFS

static ssize_t warn_count_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *page)
{
	return sysfs_emit(page, "%d\n", atomic_read(&warn_count));
}

static struct kobj_attribute warn_count_attr = __ATTR_RO(warn_count);

static __init int kernel_panic_sysfs_init(void)
{
	sysfs_add_file_to_group(kernel_kobj, &warn_count_attr.attr, NULL);
	return 0;
}
late_initcall(kernel_panic_sysfs_init);

#endif /* CONFIG_SYSFS */

/* ═══════════════════════════════════════════════════════════════════════════
 * Panic blink callback
 * ═════════════════════════════════════════════════════════════════════════*/

static long no_blink(int state)
{
	return 0;
}

/*
 * panic_blink — architecture-provided LED/console blink function.
 *
 * Called in a tight loop during the panic countdown.  The function
 * receives the current blink state (0 or 1) and returns how many
 * extra milliseconds it waited.  Defaults to no_blink() if not set
 * by platform code.
 */
long (*panic_blink)(int state);
EXPORT_SYMBOL(panic_blink);

/* ═══════════════════════════════════════════════════════════════════════════
 * Weak CPU-stop hooks
 *
 * Architectures override these to perform crash-dump preparation
 * (e.g. saving register state, disabling virtualisation extensions)
 * before halting a CPU.
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * panic_smp_self_stop — halt this CPU during panic.
 *
 * Default: spin forever calling cpu_relax().  Overridden by architectures
 * that need to perform teardown before parking the CPU.
 */
void __weak __noreturn panic_smp_self_stop(void)
{
	while (1)
		cpu_relax();
}

/**
 * nmi_panic_self_stop — halt this CPU when another has already panicked
 *                        and we are in NMI context.
 *
 * Default: delegates to panic_smp_self_stop().  Architectures with NMI
 * crash-dump support override this to save per-CPU register state.
 */
void __weak __noreturn nmi_panic_self_stop(struct pt_regs *regs)
{
	panic_smp_self_stop();
}

/**
 * crash_smp_send_stop — ask all non-panicking CPUs to stop.
 *
 * May be called twice on the panic path; protected by a static flag.
 * Default uses smp_send_stop(), which may not be fully hardened for
 * use in a panic context.  Architectures supporting crash dumps override
 * this with a more robust implementation.
 */
void __weak crash_smp_send_stop(void)
{
	static int cpus_stopped;

	if (cpus_stopped)
		return;

	smp_send_stop();
	cpus_stopped = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * panic_cpu state  —  tracks which CPU "owns" the panic path
 *
 * panic_cpu        : CPU currently executing panic(), or PANIC_CPU_INVALID.
 * panic_redirect_cpu: CPU that issued the redirect request, used to suppress
 *                     a duplicate stack dump on the target CPU.
 * ═════════════════════════════════════════════════════════════════════════*/

atomic_t panic_cpu          = ATOMIC_INIT(PANIC_CPU_INVALID);
atomic_t panic_redirect_cpu = ATOMIC_INIT(PANIC_CPU_INVALID);

/* ═══════════════════════════════════════════════════════════════════════════
 * panic_force_cpu — redirect panic to a designated CPU
 *
 * Some platforms require the crash kernel to be invoked from a specific
 * physical CPU.  When "panic_force_cpu=<n>" is passed on the kernel command
 * line, any CPU that enters panic() but is not CPU <n> will IPI (or NMI)
 * CPU <n>, pass it the formatted message, and park itself.
 *
 * Only compiled when both CONFIG_SMP and CONFIG_CRASH_DUMP are set.
 * ═════════════════════════════════════════════════════════════════════════*/

#if defined(CONFIG_SMP) && defined(CONFIG_CRASH_DUMP)

/* Dynamically allocated message buffer; NULL until late_initcall. */
static char *panic_force_buf;

static int __init panic_force_cpu_setup(char *str)
{
	int cpu;

	if (!str)
		return -EINVAL;

	if (kstrtoint(str, 0, &cpu) || cpu < 0 || cpu >= nr_cpu_ids) {
		pr_warn("panic_force_cpu: invalid value '%s'\n", str);
		return -EINVAL;
	}

	panic_force_cpu = cpu;
	return 0;
}
early_param("panic_force_cpu", panic_force_cpu_setup);

static int __init panic_force_cpu_late_init(void)
{
	if (panic_force_cpu < 0)
		return 0;

	panic_force_buf = kmalloc(PANIC_MSG_BUFSZ, GFP_KERNEL);
	return 0;
}
late_initcall(panic_force_cpu_late_init);

static void do_panic_on_target_cpu(void *info)
{
	panic("%s", (char *)info);
}

/**
 * panic_smp_redirect_cpu - Deliver a panic to @target_cpu.
 * @target_cpu: CPU that should handle the panic.
 * @msg:        Formatted panic message to deliver.
 *
 * Default implementation uses an asynchronous IPI.  Architectures with
 * NMI support can override this for more reliable delivery when the
 * target CPU may be spinning with interrupts disabled.
 *
 * Return: 0 on success, negative errno on failure.
 */
int __weak panic_smp_redirect_cpu(int target_cpu, void *msg)
{
	static call_single_data_t panic_csd;

	panic_csd.func = do_panic_on_target_cpu;
	panic_csd.info = msg;

	return smp_call_function_single_async(target_cpu, &panic_csd);
}

/**
 * panic_try_force_cpu - Redirect panic to the CPU specified by panic_force_cpu=.
 * @fmt:  Panic format string.
 * @args: Format arguments.
 *
 * Called early in vpanic() before the current CPU claims the panic_cpu
 * token.  If redirection succeeds this function does not return — the
 * calling CPU parks itself via panic_smp_self_stop().
 *
 * Returns false if the panic should proceed on the current CPU:
 *   - panic_force_cpu not configured
 *   - already on the target CPU
 *   - target CPU offline
 *   - another panic already in progress
 *   - lost the atomic redirect race
 *   - IPI delivery failed
 */
__printf(1, 0)
static bool panic_try_force_cpu(const char *fmt, va_list args)
{
	int this_cpu = raw_smp_processor_id();
	int old_cpu  = PANIC_CPU_INVALID;
	const char *msg;

	if (panic_force_cpu < 0)
		return false;

	if (this_cpu == panic_force_cpu)
		return false;

	if (!cpu_online(panic_force_cpu)) {
		pr_warn("panic: target CPU %d is offline, continuing on CPU %d\n",
			panic_force_cpu, this_cpu);
		return false;
	}

	if (panic_in_progress())
		return false;

	/*
	 * Claim the redirect slot atomically.  If another CPU got here first,
	 * fall through and let this CPU proceed normally.
	 */
	if (!atomic_try_cmpxchg(&panic_redirect_cpu, &old_cpu, this_cpu))
		return false;

	/*
	 * Format the message into panic_force_buf if available; fall back to
	 * a static string for very early boot panics or allocation failures.
	 */
	if (panic_force_buf) {
		vsnprintf(panic_force_buf, PANIC_MSG_BUFSZ, fmt, args);
		msg = panic_force_buf;
	} else {
		msg = "Redirected panic (buffer unavailable)";
	}

	console_verbose();
	bust_spinlocks(1);

	pr_emerg("panic: Redirecting from CPU %d to CPU %d for crash kernel.\n",
		 this_cpu, panic_force_cpu);

	/* Dump the originating CPU's stack before redirecting. */
	if (!test_taint(TAINT_DIE) &&
	    oops_in_progress <= 1 &&
	    IS_ENABLED(CONFIG_DEBUG_BUGVERBOSE))
		dump_stack();

	if (panic_smp_redirect_cpu(panic_force_cpu, (void *)msg) != 0) {
		/* IPI failed — give up on redirect, continue on this CPU. */
		atomic_set(&panic_redirect_cpu, PANIC_CPU_INVALID);
		pr_warn("panic: failed to redirect to CPU %d, continuing on CPU %d\n",
			panic_force_cpu, this_cpu);
		return false;
	}

	/* IPI sent successfully; this CPU should now park. */
	return true;
}

#else /* !(CONFIG_SMP && CONFIG_CRASH_DUMP) */

__printf(1, 0)
static inline bool panic_try_force_cpu(const char *fmt, va_list args)
{
	return false;
}

#endif /* CONFIG_SMP && CONFIG_CRASH_DUMP */

/* ═══════════════════════════════════════════════════════════════════════════
 * panic_cpu ownership helpers
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * panic_try_start — Try to claim ownership of the panic path.
 *
 * Uses an atomic cmpxchg to ensure only one CPU executes the core panic
 * sequence.  Also used by crash_kexec() to prevent concurrent execution.
 *
 * Returns true if this CPU successfully claimed ownership.
 */
bool panic_try_start(void)
{
	int old_cpu  = PANIC_CPU_INVALID;
	int this_cpu = raw_smp_processor_id();

	return atomic_try_cmpxchg(&panic_cpu, &old_cpu, this_cpu);
}
EXPORT_SYMBOL(panic_try_start);

/**
 * panic_reset — Release the panic_cpu ownership token.
 *
 * Used by crash_kexec() after it has finished; not normally called from
 * the panic() path.
 */
void panic_reset(void)
{
	atomic_set(&panic_cpu, PANIC_CPU_INVALID);
}
EXPORT_SYMBOL(panic_reset);

/**
 * panic_in_progress — True if any CPU is currently executing panic().
 */
bool panic_in_progress(void)
{
	return unlikely(atomic_read(&panic_cpu) != PANIC_CPU_INVALID);
}
EXPORT_SYMBOL(panic_in_progress);

/**
 * panic_on_this_cpu — True if this CPU is the one executing panic().
 *
 * Safe to call without locking: once panic_cpu is set, the task cannot
 * migrate away from (or onto) that CPU.
 */
bool panic_on_this_cpu(void)
{
	return unlikely(atomic_read(&panic_cpu) == raw_smp_processor_id());
}
EXPORT_SYMBOL(panic_on_this_cpu);

/**
 * panic_on_other_cpu — True if a different CPU is currently in panic().
 *
 * When this returns true the local CPU should release any printing
 * resources needed by the panicking CPU.
 */
bool panic_on_other_cpu(void)
{
	return panic_in_progress() && !panic_on_this_cpu();
}
EXPORT_SYMBOL(panic_on_other_cpu);

/* ═══════════════════════════════════════════════════════════════════════════
 * NMI-safe panic entry point
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * nmi_panic — Panic from NMI context.
 * @regs: Saved registers (may be NULL).
 * @msg:  Panic message.
 *
 * If this CPU successfully claims the panic token, invokes panic() normally.
 * If another CPU already panicked, parks via nmi_panic_self_stop() which
 * may save register state for crash dumps.
 */
void nmi_panic(struct pt_regs *regs, const char *msg)
{
	if (panic_try_start())
		panic("%s", msg);
	else if (panic_on_other_cpu())
		nmi_panic_self_stop(regs);
}
EXPORT_SYMBOL(nmi_panic);

/* ═══════════════════════════════════════════════════════════════════════════
 * WARN threshold check
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * check_panic_on_warn — Panic if panic_on_warn or warn_limit is exceeded.
 * @origin: Short string identifying the caller (e.g. "kernel", "kasan").
 */
void check_panic_on_warn(const char *origin)
{
	unsigned int limit;

	if (panic_on_warn)
		panic("%s: panic_on_warn set ...\n", origin);

	limit = READ_ONCE(warn_limit);
	if (atomic_inc_return(&warn_count) >= limit && limit)
		panic("%s: system warned too often (kernel.warn_limit is %d)",
		      origin, limit);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * All-CPU backtrace trigger
 * ═════════════════════════════════════════════════════════════════════════*/

static void panic_trigger_all_cpu_backtrace(void)
{
	/*
	 * Briefly allow non-panicking CPUs to write their backtraces.
	 * This flag is checked in the per-CPU backtrace handler to decide
	 * whether it may print while panic_cpu is set.
	 */
	panic_triggering_all_cpu_backtrace = true;

	if (panic_this_cpu_backtrace_printed)
		trigger_allbutcpu_cpu_backtrace(raw_smp_processor_id());
	else
		trigger_all_cpu_backtrace();

	panic_triggering_all_cpu_backtrace = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Secondary CPU shutdown
 *
 * Must be called before crash_kexec() so that NMI backtraces (if requested)
 * can still reach all CPUs before they are stopped.
 * ═════════════════════════════════════════════════════════════════════════*/

static void panic_other_cpus_shutdown(bool crash_kexec)
{
	if (panic_print & SYS_INFO_ALL_BT)
		panic_trigger_all_cpu_backtrace();

	/*
	 * Use crash_smp_send_stop() when we intend to invoke crash_kexec()
	 * afterwards, as it is expected to be more robust (e.g. NMI-based)
	 * and may save per-CPU state required by the crash kernel.
	 * Fall back to the standard smp_send_stop() otherwise.
	 */
	if (!crash_kexec)
		smp_send_stop();
	else
		crash_smp_send_stop();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Core panic implementation
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * vpanic — Halt the system with a diagnostic message.
 * @fmt:  printf-style format string.
 * @args: Argument list for @fmt.
 *
 * This is the heart of the panic() infrastructure.  The sequence is:
 *
 *  1. Suppress further WARN()-induced panics on this thread.
 *  2. Disable local IRQs and preemption.
 *  3. Optionally redirect to panic_force_cpu (CONFIG_SMP + CONFIG_CRASH_DUMP).
 *  4. Claim exclusive ownership of the panic path via panic_cpu.
 *  5. Print the panic message and (optionally) a stack dump.
 *  6. Invoke kgdb, then optionally crash_kexec() (pre-notifiers).
 *  7. Stop all other CPUs.
 *  8. Run panic_notifier_list, print sys_info, dump kernel log.
 *  9. Optionally crash_kexec() (post-notifiers).
 * 10. Flush console buffers.
 * 11. Countdown and emergency_restart(), or spin forever.
 *
 * This function never returns.
 */
void vpanic(const char *fmt, va_list args)
{
	static char buf[PANIC_MSG_BUFSZ];
	long i, i_next = 0, len;
	int state = 0;
	bool _crash_kexec_post_notifiers = crash_kexec_post_notifiers;

	/*
	 * Clear panic_on_warn for this thread.  A WARN() triggered inside a
	 * panic handler would otherwise recurse, but all other threads are
	 * blocked below by the panic_cpu mutex.
	 */
	if (panic_on_warn)
		panic_on_warn = 0;

	/*
	 * Disable interrupts to prevent an interrupt handler running after
	 * we set panic_cpu from calling panic() again and deadlocking on
	 * panic_smp_self_stop().
	 */
	local_irq_disable();
	preempt_disable_notrace();

	/*
	 * Attempt to redirect to panic_force_cpu if configured.  On success
	 * this CPU marks itself offline and calls panic_smp_self_stop().
	 */
	if (panic_try_force_cpu(fmt, args)) {
		set_cpu_online(smp_processor_id(), false);
		panic_smp_self_stop();
	}

	/*
	 * Race for the panic_cpu token.  Only one CPU proceeds past here;
	 * all others either stop themselves or wait for smp_send_stop().
	 *
	 * Note: panic_try_start() returning false with panic_on_this_cpu()
	 * true means we arrived here from nmi_panic() which already set
	 * panic_cpu for this CPU — that is also an acceptable "win".
	 */
	if (panic_try_start()) {
		/* This CPU claimed the token — proceed. */
	} else if (panic_on_other_cpu()) {
		panic_smp_self_stop();
	}

	console_verbose();
	bust_spinlocks(1);

	len = vscnprintf(buf, sizeof(buf), fmt, args);
	/* Strip trailing newline — pr_emerg adds its own. */
	if (len && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	pr_emerg("Kernel panic - not syncing: %s\n", buf);

	/*
	 * Stack dump — suppressed in three cases:
	 *   a) We are the redirected panic target (the originating CPU
	 *      already dumped its stack before sending the IPI).
	 *   b) We are in a nested panic (TAINT_DIE set or oops_in_progress>1).
	 *   c) CONFIG_DEBUG_BUGVERBOSE is disabled.
	 */
	if (atomic_read(&panic_redirect_cpu) != PANIC_CPU_INVALID &&
	    panic_force_cpu == raw_smp_processor_id()) {
		pr_emerg("panic: Redirected from CPU %d, skipping stack dump.\n",
			 atomic_read(&panic_redirect_cpu));
	} else if (test_taint(TAINT_DIE) || oops_in_progress > 1) {
		panic_this_cpu_backtrace_printed = true;
	} else if (IS_ENABLED(CONFIG_DEBUG_BUGVERBOSE)) {
		dump_stack();
		panic_this_cpu_backtrace_printed = true;
	}

	/*
	 * Give kgdb a chance to attach before we stop the other CPUs.
	 * Without this window, processes still running on other CPUs cannot
	 * be inspected by a remote debugger.
	 */
	kgdb_panic(buf);

	/*
	 * Pre-notifier kdump path:  if crash_kexec_post_notifiers is false
	 * (the default), boot the crash kernel now — before notifiers run.
	 * This maximises the chance of a clean capture even if a notifier
	 * causes further instability.
	 */
	if (!_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	panic_other_cpus_shutdown(_crash_kexec_post_notifiers);

	printk_legacy_allow_panic_sync();

	/* Run registered panic handlers (may add kmsg annotations). */
	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	sys_info(panic_print);

	kmsg_dump_desc(KMSG_DUMP_PANIC, buf);

	/*
	 * Post-notifier kdump path: if crash_kexec_post_notifiers is true,
	 * boot the crash kernel now.  Note that notifiers may have made the
	 * kernel less stable, increasing the risk of a failed capture.
	 */
	if (_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	console_unblank();

	/*
	 * The CPU that was stopped by smp_send_stop() may have held a console
	 * lock with data still in the buffer.  Flush here regardless of whether
	 * we can acquire the lock; debug_locks_off() suppresses lock-imbalance
	 * warnings for this deliberate release.
	 */
	debug_locks_off();
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	/* Optionally replay the full console log so it appears after the panic banner. */
	if ((panic_print & SYS_INFO_PANIC_CONSOLE_REPLAY) || panic_console_replay)
		console_flush_on_panic(CONSOLE_REPLAY_ALL);

	if (!panic_blink)
		panic_blink = no_blink;

	if (panic_timeout > 0) {
		pr_emerg("Rebooting in %d seconds..\n", panic_timeout);

		for (i = 0; i < panic_timeout * 1000; i += PANIC_TIMER_STEP) {
			touch_nmi_watchdog();
			if (i >= i_next) {
				i += panic_blink(state ^= 1);
				i_next = i + 3600 / PANIC_BLINK_SPD;
			}
			mdelay(PANIC_TIMER_STEP);
		}
	}

	if (panic_timeout != 0) {
		/*
		 * This will not be a graceful shutdown, but if there is any
		 * chance of rebooting the system we take it.
		 */
		if (panic_reboot_mode != REBOOT_UNDEFINED)
			reboot_mode = panic_reboot_mode;
		emergency_restart();
	}

#ifdef __sparc__
	{
		extern int stop_a_enabled;
		stop_a_enabled = 1;
		pr_emerg("Press Stop-A (L1-A) from sun keyboard or send break\n"
			 "twice on console to return to the boot prom\n");
	}
#endif
#if defined(CONFIG_S390)
	disabled_wait();
#endif

	pr_emerg("---[ end Kernel panic - not syncing: %s ]---\n", buf);

	/* Prevent important messages above from scrolling off screen. */
	suppress_printk = 1;

	/*
	 * Final flush: messages may not yet be visible if we are in a
	 * deferred-print context (e.g. NMI) and irq_work is unavailable.
	 */
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);
	nbcon_atomic_flush_unsafe();

	local_irq_enable();

	/* Spin forever, blinking the panic indicator. */
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
}
EXPORT_SYMBOL(vpanic);

/**
 * panic — Halt the system with a formatted diagnostic message.
 * @fmt: printf-style format string followed by optional arguments.
 *
 * Thin variadic wrapper around vpanic().  This function never returns.
 */
void panic(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vpanic(fmt, args);
	va_end(args);
}
EXPORT_SYMBOL(panic);

/* ═══════════════════════════════════════════════════════════════════════════
 * Taint flags
 *
 * To add a new flag:
 *   1. Add TAINT_FLAG() entry below.
 *   2. Update tools/debugging/kernel-chktaint.
 *   3. Update Documentation/admin-guide/tainted-kernels.rst.
 *   4. Adjust INIT_TAINT_BUF_MAX if the verbose string grows.
 * ═════════════════════════════════════════════════════════════════════════*/

#define TAINT_FLAG(taint, _c_true, _c_false)		\
	[TAINT_##taint] = {				\
		.c_true  = _c_true,			\
		.c_false = _c_false,			\
		.desc    = #taint,			\
	}

const struct taint_flag taint_flags[TAINT_FLAGS_COUNT] = {
	TAINT_FLAG(PROPRIETARY_MODULE,		'P', 'G'),
	TAINT_FLAG(FORCED_MODULE,		'F', ' '),
	TAINT_FLAG(CPU_OUT_OF_SPEC,		'S', ' '),
	TAINT_FLAG(FORCED_RMMOD,		'R', ' '),
	TAINT_FLAG(MACHINE_CHECK,		'M', ' '),
	TAINT_FLAG(BAD_PAGE,			'B', ' '),
	TAINT_FLAG(USER,			'U', ' '),
	TAINT_FLAG(DIE,				'D', ' '),
	TAINT_FLAG(OVERRIDDEN_ACPI_TABLE,	'A', ' '),
	TAINT_FLAG(WARN,			'W', ' '),
	TAINT_FLAG(CRAP,			'C', ' '),
	TAINT_FLAG(FIRMWARE_WORKAROUND,		'I', ' '),
	TAINT_FLAG(OOT_MODULE,			'O', ' '),
	TAINT_FLAG(UNSIGNED_MODULE,		'E', ' '),
	TAINT_FLAG(SOFTLOCKUP,			'L', ' '),
	TAINT_FLAG(LIVEPATCH,			'K', ' '),
	TAINT_FLAG(AUX,				'X', ' '),
	TAINT_FLAG(RANDSTRUCT,			'T', ' '),
	TAINT_FLAG(TEST,			'N', ' '),
	TAINT_FLAG(FWCTL,			'J', ' '),
};

#undef TAINT_FLAG

/* ───────────────────────────────────────────────────────────────────────────
 * Taint string formatting
 * ─────────────────────────────────────────────────────────────────────────*/

static void print_tainted_seq(struct seq_buf *s, bool verbose)
{
	const char *sep = "";
	int i;

	if (!tainted_mask) {
		seq_buf_puts(s, "Not tainted");
		return;
	}

	seq_buf_printf(s, "Tainted: ");

	for (i = 0; i < TAINT_FLAGS_COUNT; i++) {
		const struct taint_flag *t = &taint_flags[i];
		bool is_set = test_bit(i, &tainted_mask);
		char c = is_set ? t->c_true : t->c_false;

		if (verbose) {
			if (is_set) {
				seq_buf_printf(s, "%s[%c]=%s", sep, c, t->desc);
				sep = ", ";
			}
		} else {
			seq_buf_putc(s, c);
		}
	}
}

/*
 * Taint string buffer management.
 *
 * The initial static buffer handles all taint flags in verbose mode with
 * headroom.  After the slab allocator is available an exact-sized buffer is
 * allocated; the static buffer remains as a fallback on allocation failure.
 *
 * INIT_TAINT_BUF_MAX must be large enough for the current verbose taint
 * string (≈327 characters as of this writing) plus a safety margin.
 */
#define INIT_TAINT_BUF_MAX 350

static char init_taint_buf[INIT_TAINT_BUF_MAX] __initdata;
static char *taint_buf      __refdata = init_taint_buf;
static size_t taint_buf_size          = INIT_TAINT_BUF_MAX;

static __init int alloc_taint_buf(void)
{
	size_t size = sizeof("Tainted: ") - 1;
	char *buf;
	int i;

	for (i = 0; i < TAINT_FLAGS_COUNT; i++) {
		size += 2;				/* ", "    */
		size += 4;				/* "[%c]="  */
		size += strlen(taint_flags[i].desc);
	}
	size += 1;					/* NUL      */

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		panic("Failed to allocate taint string buffer");

	taint_buf      = buf;
	taint_buf_size = size;
	return 0;
}
postcore_initcall(alloc_taint_buf);

static const char *_print_tainted(bool verbose)
{
	struct seq_buf s;

	BUILD_BUG_ON(ARRAY_SIZE(taint_flags) != TAINT_FLAGS_COUNT);

	seq_buf_init(&s, taint_buf, taint_buf_size);
	print_tainted_seq(&s, verbose);
	return seq_buf_str(&s);
}

/**
 * print_tainted — Return a compact taint string.
 *
 * The string is overwritten on the next call to print_tainted() or
 * print_tainted_verbose().  Always NUL-terminated.
 *
 * See Documentation/admin-guide/sysctl/kernel.rst for flag meanings.
 */
const char *print_tainted(void)
{
	return _print_tainted(false);
}

/**
 * print_tainted_verbose — Return a verbose taint string.
 *
 * Like print_tainted() but includes the flag name alongside each letter,
 * e.g. "[P]=PROPRIETARY_MODULE, [O]=OOT_MODULE".
 */
const char *print_tainted_verbose(void)
{
	return _print_tainted(true);
}

int test_taint(unsigned flag)
{
	return test_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(test_taint);

unsigned long get_taint(void)
{
	return tainted_mask;
}

/**
 * add_taint — Mark the kernel as tainted.
 * @flag:       One of the TAINT_* constants.
 * @lockdep_ok: LOCKDEP_STILL_OK if lock debugging remains reliable;
 *              LOCKDEP_NOW_UNRELIABLE to disable lock debugging.
 *
 * If the new taint matches panic_on_taint, the kernel panics immediately.
 */
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
	if (lockdep_ok == LOCKDEP_NOW_UNRELIABLE && __debug_locks_off())
		pr_warn("Disabling lock debugging due to kernel taint\n");

	set_bit(flag, &tainted_mask);

	if (tainted_mask & panic_on_taint) {
		panic_on_taint = 0;
		panic("panic_on_taint set ...");
	}
}
EXPORT_SYMBOL(add_taint);

/* ═══════════════════════════════════════════════════════════════════════════
 * Oops serialisation
 *
 * When pause_on_oops is set, concurrent oops events are serialised so that
 * each gets a chance to print without interleaving.  The first CPU to enter
 * is designated the "printer"; all others spin until the printer finishes
 * or the pause_on_oops timeout elapses.
 * ═════════════════════════════════════════════════════════════════════════*/

static void spin_msec(int msecs)
{
	int i;

	for (i = 0; i < msecs; i++) {
		touch_nmi_watchdog();
		mdelay(1);
	}
}

static void do_oops_enter_exit(void)
{
	unsigned long flags;
	static int spin_counter;

	if (!pause_on_oops)
		return;

	spin_lock_irqsave(&pause_on_oops_lock, flags);

	if (pause_on_oops_flag == 0) {
		/* This CPU is first; it may print immediately. */
		pause_on_oops_flag = 1;
	} else {
		if (!spin_counter) {
			/* This CPU becomes the countdown timer. */
			spin_counter = pause_on_oops;
			do {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(MSEC_PER_SEC);
				spin_lock(&pause_on_oops_lock);
			} while (--spin_counter);
			pause_on_oops_flag = 0;
		} else {
			/* Another CPU is already counting; just wait. */
			while (spin_counter) {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(1);
				spin_lock(&pause_on_oops_lock);
			}
		}
	}

	spin_unlock_irqrestore(&pause_on_oops_lock, flags);
}

/**
 * oops_may_print — True if this CPU is currently allowed to print oops info.
 */
bool oops_may_print(void)
{
	return pause_on_oops_flag == 0;
}

/**
 * oops_enter — Called by arch oops handlers before printing anything.
 *
 * Disables tracing and lock debugging (the kernel's integrity can no longer
 * be trusted), serialises concurrent oopses, and optionally triggers an
 * all-CPU backtrace.
 */
void oops_enter(void)
{
	nbcon_cpu_emergency_enter();
	tracing_off();
	debug_locks_off();
	do_oops_enter_exit();

	if (sysctl_oops_all_cpu_backtrace)
		trigger_all_cpu_backtrace();
}

static void print_oops_end_marker(void)
{
	pr_warn("---[ end trace %016llx ]---\n", 0ULL);
}

/**
 * oops_exit — Called by arch oops handlers after printing.
 *
 * Releases the oops serialisation lock, prints the end marker, dumps the
 * kernel log, and exits the nbcon emergency section.
 */
void oops_exit(void)
{
	do_oops_enter_exit();
	print_oops_end_marker();
	nbcon_cpu_emergency_exit();
	kmsg_dump(KMSG_DUMP_OOPS);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WARN implementation
 * ═════════════════════════════════════════════════════════════════════════*/

struct warn_args {
	const char *fmt;
	va_list args;
};

/**
 * __warn — Core WARN() implementation.
 * @file:   Source file (may be NULL for assembly callers).
 * @line:   Source line number.
 * @caller: Return address of the WARN site.
 * @taint:  Taint flag to apply (e.g. TAINT_WARN).
 * @regs:   Saved registers, or NULL.
 * @args:   Optional printf arguments for the warning message.
 */
void __warn(const char *file, int line, void *caller, unsigned taint,
	    struct pt_regs *regs, struct warn_args *args)
{
	nbcon_cpu_emergency_enter();
	disable_trace_on_warning();

	if (file)
		pr_warn("WARNING: %s:%d at %pS, CPU#%d: %s/%d\n",
			file, line, caller,
			raw_smp_processor_id(), current->comm, current->pid);
	else
		pr_warn("WARNING: at %pS, CPU#%d: %s/%d\n",
			caller,
			raw_smp_processor_id(), current->comm, current->pid);

#pragma GCC diagnostic push
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wsuggest-attribute=format"
#endif
	if (args)
		vprintk(args->fmt, args->args);
#pragma GCC diagnostic pop

	print_modules();

	if (regs)
		show_regs(regs);

	check_panic_on_warn("kernel");

	if (!regs)
		dump_stack();

	print_irqtrace_events(current);
	print_oops_end_marker();
	trace_error_report_end(ERROR_DETECTOR_WARN, (unsigned long)caller);

	/* Taint the kernel but do not disturb lock debugging. */
	add_taint(taint, LOCKDEP_STILL_OK);

	nbcon_cpu_emergency_exit();
}

/* ───────────────────────────────────────────────────────────────────────────
 * WARN slow paths
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_BUG
# ifndef __WARN_FLAGS

void warn_slowpath_fmt(const char *file, int line, unsigned taint,
		       const char *fmt, ...)
{
	bool rcu = warn_rcu_enter();
	struct warn_args args;

	pr_warn(CUT_HERE);

	if (!fmt) {
		__warn(file, line, __builtin_return_address(0),
		       taint, NULL, NULL);
		warn_rcu_exit(rcu);
		return;
	}

	args.fmt = fmt;
	va_start(args.args, fmt);
	__warn(file, line, __builtin_return_address(0), taint, NULL, &args);
	va_end(args.args);
	warn_rcu_exit(rcu);
}
EXPORT_SYMBOL(warn_slowpath_fmt);

# else /* __WARN_FLAGS */

void __warn_printk(const char *fmt, ...)
{
	bool rcu = warn_rcu_enter();
	va_list args;

	pr_warn(CUT_HERE);

	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
	warn_rcu_exit(rcu);
}
EXPORT_SYMBOL(__warn_printk);

# endif /* __WARN_FLAGS */

/* ───────────────────────────────────────────────────────────────────────────
 * WARN_ONCE reset via debugfs
 * ─────────────────────────────────────────────────────────────────────────*/

static int clear_warn_once_set(void *data, u64 val)
{
	generic_bug_clear_once();
	memset(__start_once, 0, __end_once - __start_once);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clear_warn_once_fops, NULL, clear_warn_once_set,
			 "%lld\n");

static __init int register_warn_debugfs(void)
{
	debugfs_create_file_unsafe("clear_warn_once", 0200, NULL, NULL,
				   &clear_warn_once_fops);
	return 0;
}
device_initcall(register_warn_debugfs);

#endif /* CONFIG_BUG */

/* ═══════════════════════════════════════════════════════════════════════════
 * Stack-protector failure handler
 * ═════════════════════════════════════════════════════════════════════════*/

#ifdef CONFIG_STACKPROTECTOR
/**
 * __stack_chk_fail — Called by GCC's -fstack-protector on canary mismatch.
 *
 * The on-stack canary value has been corrupted, indicating a stack buffer
 * overflow.  Panic immediately — recovery is not possible.
 */
__visible noinstr void __stack_chk_fail(void)
{
	unsigned long flags;

	instrumentation_begin();
	flags = user_access_save();

	panic("stack-protector: Kernel stack is corrupted in: %pB",
	      __builtin_return_address(0));

	user_access_restore(flags);
	instrumentation_end();
}
EXPORT_SYMBOL(__stack_chk_fail);
#endif /* CONFIG_STACKPROTECTOR */

/* ═══════════════════════════════════════════════════════════════════════════
 * Core kernel parameters
 * ═════════════════════════════════════════════════════════════════════════*/

core_param(panic,                     panic_timeout,              int,  0644);
core_param(pause_on_oops,             pause_on_oops,              int,  0644);
core_param(panic_on_warn,             panic_on_warn,              int,  0644);
core_param(crash_kexec_post_notifiers,crash_kexec_post_notifiers, bool, 0644);
core_param(panic_console_replay,      panic_console_replay,       bool, 0644);

/* panic_print is deprecated; reads/writes go through the _ops wrapper. */
static int panic_print_set(const char *val, const struct kernel_param *kp)
{
	panic_print_deprecated();
	return param_set_ulong(val, kp);
}

static int panic_print_get(char *val, const struct kernel_param *kp)
{
	return param_get_ulong(val, kp);
}

static const struct kernel_param_ops panic_print_ops = {
	.set = panic_print_set,
	.get = panic_print_get,
};
__core_param_cb(panic_print, &panic_print_ops, &panic_print, 0644);

/* ═══════════════════════════════════════════════════════════════════════════
 * Early boot parameters
 * ═════════════════════════════════════════════════════════════════════════*/

static int __init oops_setup(char *s)
{
	if (!s)
		return -EINVAL;
	if (!strcmp(s, "panic"))
		panic_on_oops = 1;
	return 0;
}
early_param("oops", oops_setup);

static int __init panic_on_taint_setup(char *s)
{
	char *taint_str;

	if (!s)
		return -EINVAL;

	taint_str = strsep(&s, ",");
	if (kstrtoul(taint_str, 16, &panic_on_taint))
		return -EINVAL;

	/* Mask off any out-of-range bits. */
	panic_on_taint &= TAINT_FLAGS_MAX;

	if (!panic_on_taint)
		return -EINVAL;

	if (s && !strcmp(s, "nousertaint"))
		panic_on_taint_nousertaint = true;

	pr_info("panic_on_taint: bitmask=0x%lx nousertaint_mode=%s\n",
		panic_on_taint,
		str_enabled_disabled(panic_on_taint_nousertaint));

	return 0;
}
early_param("panic_on_taint", panic_on_taint_setup);
