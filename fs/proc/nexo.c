// SPDX-License-Identifier: GPL-2.0
/*
 * Nexo OS - Core Gaming Interface
 * Location: /proc/nexo
 * Purpose: Central control for Game Mode, Boost Levels and Performance Stats.
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/nexo_perf.h> // Bevat de globale vlaggen

/* Nexo Globals (gedefinieerd in sched/core.c of power/main.c) */
extern bool nexo_game_mode_active;
extern int nexo_boost_level;
extern unsigned long nexo_last_latency_spike;

/* --- 1. Game Mode Interface --- */
static int nexo_gamemode_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", nexo_game_mode_active);
	return 0;
}

static ssize_t nexo_gamemode_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char input[2];
	if (copy_from_user(input, buf, min(count, sizeof(input) - 1)))
		return -EFAULT;

	if (input[0] == '1') {
		nexo_game_mode_active = true;
		pr_info("Nexo OS: Game Mode ENABLED. Power management bypassed.\n");
	} else {
		nexo_game_mode_active = false;
		pr_info("Nexo OS: Game Mode DISABLED. Power saving restored.\n");
	}
	return count;
}

/* --- 2. Boost Level Interface (0-3) --- */
static int nexo_boost_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", nexo_boost_level);
	return 0;
}

static ssize_t nexo_boost_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	int val;
	if (kstrtoint_from_user(buf, count, 10, &val))
		return -EINVAL;

	if (val >= 0 && val <= 3) {
		nexo_boost_level = val;
		/* Hier zouden we direct de CPU frequency governor kunnen triggeren */
		pr_info("Nexo OS: Boost Level set to %d\n", val);
	}
	return count;
}

/* --- 3. Stats Interface --- */
static int nexo_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "--- Nexo OS Performance Stats ---\n");
	seq_printf(m, "Game Mode Status: %s\n", nexo_game_mode_active ? "ACTIVE" : "IDLE");
	seq_printf(m, "Current Boost:    %d\n", nexo_boost_level);
	seq_printf(m, "Last Max Latency: %lu us\n", nexo_last_latency_spike);
	seq_printf(m, "Scheduler Class:  Nexo_Turbo_v1\n");
	return 0;
}

/* Proc Operations Definitions */
static int nexo_gamemode_open(struct inode *inode, struct file *file) {
	return single_open(file, nexo_gamemode_show, NULL);
}

static int nexo_boost_open(struct inode *inode, struct file *file) {
	return single_open(file, nexo_boost_show, NULL);
}

static int nexo_stats_open(struct inode *inode, struct file *file) {
	return single_open(file, nexo_stats_show, NULL);
}

static const struct proc_ops nexo_gamemode_ops = {
	.proc_open    = nexo_gamemode_open,
	.proc_read    = seq_read,
	.proc_write   = nexo_gamemode_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops nexo_boost_ops = {
	.proc_open    = nexo_boost_open,
	.proc_read    = seq_read,
	.proc_write   = nexo_boost_write,
	.proc_release = single_release,
};

static const struct proc_ops nexo_stats_ops = {
	.proc_open    = nexo_stats_open,
	.proc_read    = seq_read,
	.proc_release = single_release,
};

/* --- Initialisatie --- */
static int __init nexo_proc_init(void)
{
	struct proc_dir_entry *nexo_dir;

	nexo_dir = proc_mkdir("nexo", NULL);
	if (!nexo_dir)
		return -ENOMEM;

	proc_create("game_mode", 0666, nexo_dir, &nexo_gamemode_ops);
	proc_create("boost_level", 0666, nexo_dir, &nexo_boost_ops);
	proc_create("stats", 0444, nexo_dir, &nexo_stats_ops);

	pr_info("Nexo OS: /proc/nexo interface initialized.\n");
	return 0;
}

fs_initcall(nexo_proc_init);
