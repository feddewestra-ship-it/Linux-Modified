// SPDX-License-Identifier: GPL-2.0
/*
 * Nexo OS - Performance Optimized Autosleep
 * Focus: Prevent stuttering and micro-sleeps during high-load gaming.
 */

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/nexo_perf.h> // NEXO: Voor game-mode detectie

#include "power.h"

static suspend_state_t autosleep_state;
static struct workqueue_struct *autosleep_wq;
static DEFINE_MUTEX(autosleep_lock);
static struct wakeup_source *autosleep_ws;

/* NEXO: Global flag voor game-mode power management */
extern bool nexo_game_mode_active; 

static void try_to_suspend(struct work_struct *work)
{
	unsigned int initial_count, final_count;

	/* * NEXO PERFORMANCE BOOST: 
	 * Als de gebruiker aan het gamen is, negeren we de autosleep cyclus volledig.
	 * Dit voorkomt dat de power-management state-machine resources verbruikt
	 * of interrupts maskeert tijdens kritieke frames.
	 */
	if (nexo_game_mode_active) {
		pr_debug("Nexo OS: Autosleep bypassed (Game Mode Active)\n");
		goto reschedule;
	}

	if (!pm_get_wakeup_count(&initial_count, true))
		goto out;

	mutex_lock(&autosleep_lock);

	if (!pm_save_wakeup_count(initial_count) ||
		system_state != SYSTEM_RUNNING) {
		mutex_unlock(&autosleep_lock);
		goto out;
	}

	if (autosleep_state == PM_SUSPEND_ON) {
		mutex_unlock(&autosleep_lock);
		return;
	}

	/* NEXO: Extra check voor CPU load voordat we suspenden */
	if (total_cpu_load() > 10) { // Als er nog significante achtergrondactiviteit is
		mutex_unlock(&autosleep_lock);
		goto reschedule;
	}

	if (autosleep_state >= PM_SUSPEND_MAX)
		hibernate();
	else
		pm_suspend(autosleep_state);

	mutex_unlock(&autosleep_lock);

	if (!pm_get_wakeup_count(&final_count, false))
		goto out;

	if (final_count == initial_count)
		schedule_timeout_uninterruptible(HZ / 2);

out:
	queue_up_suspend_work();
	return;

reschedule:
	/* NEXO: Verleng de interval bij actieve belasting om CPU-churn te voorkomen */
	queue_delayed_work(autosleep_wq, &suspend_work, msecs_to_jiffies(5000));
}

/* NEXO: Gebruik delayed work om constante wakeups te minimaliseren */
static DECLARE_DELAYED_WORK(suspend_work, try_to_suspend);

void queue_up_suspend_work(void)
{
	if (autosleep_state > PM_SUSPEND_ON) {
		/* NEXO: Gebruik delayed queueing om de scheduler rust te geven */
		queue_delayed_work(autosleep_wq, &suspend_work, msecs_to_jiffies(100));
	}
}

suspend_state_t pm_autosleep_state(void)
{
	return autosleep_state;
}

int pm_autosleep_lock(void)
{
	return mutex_lock_interruptible(&autosleep_lock);
}

void pm_autosleep_unlock(void)
{
	mutex_unlock(&autosleep_lock);
}

int pm_autosleep_set_state(suspend_state_t state)
{
#ifndef CONFIG_HIBERNATION
	if (state >= PM_SUSPEND_MAX)
		return -EINVAL;
#endif

	__pm_stay_awake(autosleep_ws);

	mutex_lock(&autosleep_lock);

	autosleep_state = state;

	__pm_relax(autosleep_ws);

	if (state > PM_SUSPEND_ON) {
		pm_wakep_autosleep_enabled(true);
		queue_up_suspend_work();
	} else {
		pm_wakep_autosleep_enabled(false);
	}

	mutex_unlock(&autosleep_lock);
	return 0;
}

int __init pm_autosleep_init(void)
{
	autosleep_ws = wakeup_source_register(NULL, "autosleep");
	if (!autosleep_ws)
		return -ENOMEM;

	/* * NEXO BOOST: WQ_HIGHPRI zorgt dat power management taken snel worden 
	 * afgehandeld als ze nodig zijn, zodat ze niet 'hangen' en lock-contention 
	 * veroorzaken met andere kernel threads. 
	 */
	autosleep_wq = alloc_ordered_workqueue("autosleep", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (autosleep_wq)
		return 0;

	wakeup_source_unregister(autosleep_ws);
	return -ENOMEM;
}
