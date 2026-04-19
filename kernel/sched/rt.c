// SPDX-License-Identifier: GPL-2.0
/*
 * Nexo OS - Real-Time Scheduling Class (SCHED_FIFO / SCHED_RR)
 * Updated: Fallback Safety & Max Boost Limits
 */

#include "sched.h"
#include <linux/nexo_sched.h> 

/* Nexo RR System: Dynamische timeslice default */
int sched_rr_timeslice = RR_TIMESLICE;

/* * NEXO MAX BOOST LIMIT: De maximale bonus die een game-proces kan krijgen.
 * Voorkomt dat taken onbeperkt CPU-tijd 'stelen'. 
 */
#define NEXO_MAX_BOOST_PERCENT 25
#define NEXO_CPU_OVERLOAD_THRESHOLD 950 // 95% load

/* * NEXO PROTECTION & FALLBACK: 
 * Fallback-mechanisme wanneer de CPU bezwijkt onder RT druk.
 */
static inline bool nexo_rt_throttled(struct rt_rq *rt_rq)
{
	unsigned int cpu_load = cpu_util_cfs(cpu_of(rq_of_rt_rq(rt_rq)));

	if (!sysctl_sched_rt_runtime)
		return true;

	/* * NEXO FALLBACK: Als de CPU overload dreigt (>95%), 
	 * dwingen we onmiddellijke throttling af om kernel-starvation te voorkomen.
	 */
	if (unlikely(cpu_load > NEXO_CPU_OVERLOAD_THRESHOLD)) {
		return true; 
	}

	/* Nexo IRQ Awareness: Als IRQ load > 50%, wees strenger met RT runtime */
	if (nexo_irq_pressure() > 500) 
		return rt_rq->rt_time > (sysctl_sched_rt_runtime / 2);

	return rt_rq->rt_throttled;
}

/*
 * NEXO BOOST LAYER: Enqueue logica
 */
static void
enqueue_rt_entity(struct rt_rq *rt_rq, struct rt_entity *rt_se, unsigned int flags)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	/* Nexo RT Boost: Alleen boosten als we niet in een overload-state zitten */
	if ((flags & ENQUEUE_NEXO_BOOST) && nexo_irq_pressure() < 800)
		list_add(&rt_se->run_list, queue);
	else
		list_add_tail(&rt_se->run_list, queue);

	__set_bit(rt_se_prio(rt_se), array->bitmap);
	inc_rt_tasks(rt_se, rt_rq);
}

/*
 * NEXO GAME MODE & PROTECTION: Runtime accounting
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 delta_exec;

	delta_exec = rq_clock_task(rq) - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	/* * NEXO MAX BOOST LIMIT: 
	 * We geven 20% bonus aan games, maar NOOIT meer dan NEXO_MAX_BOOST_PERCENT.
	 * Als de CPU-temperatuur te hoog wordt (nexo_thermal_crit), vervalt de bonus.
	 */
	if ((curr->nexo_flags & NEXO_GAME_MODE_ACTIVE) && !nexo_thermal_crit()) {
		delta_exec = (delta_exec * 80) / 100; 
	}

	curr->se.sum_exec_runtime += delta_exec;
	curr->se.exec_start = rq_clock_task(rq);

	if (!rt_bandwidth_enabled())
		return;

	for_each_sched_rt_entity(&curr->rt) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

		if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			
			/* * NEXO FALLBACK TRIGGER:
			 * Check of deze RT-taak de CPU over de overload threshold duwt.
			 */
			if (nexo_rt_throttled(rt_rq)) {
				rt_rq->rt_throttled = 1;
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

	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	/* * NEXO DYNAMIC SLICE: 
	 * Als CPU-load hoog is, verkorten we de slices om multitasking te redden.
	 */
	p->rt.time_slice = nexo_calculate_rr_slice(rt_rq);
	if (nexo_irq_pressure() > 600)
		p->rt.time_slice /= 2;

	requeue_task_rt(rq, p, 0);
	resched_curr(rq);
}

/*
 * NEXO PICK NEXT: Balancing & Protection
 */
static int pick_next_task_rt(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct task_struct *p;
	struct rt_rq *rt_rq = &rq->rt;

	if (!rt_rq->rt_nr_running)
		return NULL;

	p = _pick_next_task_rt(rq);
	
	/* * NEXO FALLBACK RECOVERY: 
	 * Als we een taak kiezen terwijl load kritiek is, dwingen we 
	 * een IRQ boost af om de input-lag (muis/toetsenbord) te redden.
	 */
	if (p && nexo_irq_pressure() > 850) {
		nexo_boost_irq_softirqs(rq->cpu);
		/* Tijdelijke straf voor zware RT taken tijdens overload */
		if (p->rt.time_slice > 10)
			p->rt.time_slice = 10;
	}

	return p;
}
