// SPDX-License-Identifier: GPL-2.0
/*
 * Real-Time Scheduling Class (SCHED_FIFO / SCHED_RR)
 * Updated with Nexo RT Boost Layer, Game Mode & Protection
 */

#include "sched.h"
#include <linux/nexo_sched.h> // Hypothese: Nexo headers voor vlaggen

/* Nexo RR System: Dynamische timeslice default */
int sched_rr_timeslice = RR_TIMESLICE;

/* * NEXO PROTECTION: Voorkom volledige systeem starvation.
 * In plaats van harde throttling, verlagen we de impact van RT load 
 * wanneer IRQ druk (Nexo IRQ A) te hoog wordt.
 */
static inline bool nexo_rt_throttled(struct rt_rq *rt_rq)
{
	if (!sysctl_sched_rt_runtime)
		return true;

	/* Nexo IRQ Awareness: Als IRQ load > 50%, wees strenger met RT runtime */
	if (nexo_irq_pressure() > 500) // 50% pressure
		return rt_rq->rt_time > (sysctl_sched_rt_runtime / 2);

	return rt_rq->rt_throttled;
}

/*
 * NEXO BOOST LAYER: Aanpassing van enqueue logica voor lagere latency.
 */
static void
enqueue_rt_entity(struct rt_rq *rt_rq, struct rt_entity *rt_se, unsigned int flags)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	/* * Nexo RT Boost: Latency-gevoelige taken (bijv. audio of input)
	 * worden vooraan de queue geplaatst voor onmiddellijke executie.
	 */
	if (flags & ENQUEUE_NEXO_BOOST || (rt_se->boost_priority))
		list_add(&rt_se->run_list, queue);
	else
		list_add_tail(&rt_se->run_list, queue);

	__set_bit(rt_se_prio(rt_se), array->bitmap);
	inc_rt_tasks(rt_se, rt_rq);
}

/*
 * NEXO GAME MODE & IRQ AWARENESS: Runtime accounting
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct rt_rq *rt_rq = rt_rq_of_se(&curr->rt);
	u64 delta_exec;

	delta_exec = rq_clock_task(rq) - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	/* Nexo Game Mode: Verminder RT accounting druk voor game processen */
	if (curr->nexo_flags & NEXO_GAME_MODE_ACTIVE) {
		delta_exec = (delta_exec * 80) / 100; // 20% bonus runtime
	}

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq_clock_task(rq);
	cpuacct_charge(curr, delta_exec);

	/* Nexo IRQ Awareness: Monitor zware RT belasting vs IRQ */
	if (unlikely(delta_exec > 1000000ULL)) { // > 1ms in 1 burst
		nexo_log_heavy_rt(curr); 
	}

	if (!rt_bandwidth_enabled())
		return;

	for_each_sched_rt_entity(&curr->rt) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

		if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			
			/* Nexo Protection: Slimme throttling check */
			if (nexo_rt_throttled(rt_rq)) {
				if (rt_rq_throttled(rt_rq))
					resched_curr(rq);
			}
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		}
	}
}

/*
 * NEXO RR SYSTEM: Slimmere Round Robin
 */
static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct rt_rq *rt_rq = rt_rq_of_se(&p->rt);

	update_curr_rt(rq);
	update_rt_rq_load_avg(rq_clock_pelt(rq), rt_rq, 1);

	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	/* * Nexo RR: Dynamische berekening van de volgende slice.
	 * Bij veel RT taken maken we de slices korter om latency te drukken.
	 */
	p->rt.time_slice = nexo_calculate_rr_slice(rt_rq);

	/* Zet taak achteraan en forceer resched */
	requeue_task_rt(rq, p, 0);
	resched_curr(rq);
}

/*
 * NEXO IRQ A: Detectie van zware load tijdens balancing
 */
static int pick_next_task_rt(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct task_struct *p;
	struct rt_rq *rt_rq = &rq->rt;

	if (!rt_rq->rt_nr_running)
		return NULL;

	/* Nexo Latency Opt: Sla balancing over als we in een 'ultra-low-latency' window zitten */
	if (rq->nexo_latency_critical && rt_rq->rt_nr_running == 1)
		goto pick;

	if (prev && prev->sched_class == &rt_sched_class)
		update_curr_rt(rq);

pick:
	p = _pick_next_task_rt(rq);
	
	/* Nexo Protection: Als we een taak kiezen terwijl IRQ load hoog is, boost IRQ threads */
	if (p && nexo_irq_pressure() > 700)
		nexo_boost_irq_softirqs(rq->cpu);

	return p;
}
