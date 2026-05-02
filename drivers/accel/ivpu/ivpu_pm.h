/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Optimized IVPU Power Management Header (Nexo OS tuned)
 */

#ifndef __IVPU_PM_H__
#define __IVPU_PM_H__

#include <linux/types.h>
#include <linux/rwsem.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>

struct ivpu_device;

/*
 * Power management state is split into:
 * - runtime control (suspend/resume)
 * - recovery path (fault handling)
 * - reset path (fast engine reset preferred)
 */
struct ivpu_pm_info {
	struct ivpu_device *vdev;

	/* Workqueues (kept minimal latency impact) */
	struct delayed_work job_timeout_work;
	struct work_struct recovery_work;

	/* Locking: serialize full reset path only */
	struct rw_semaphore reset_lock;

	/* State tracking */
	atomic_t reset_pending;
	atomic_t engine_reset_count;

	/*
	 * NOTE (optimization):
	 * full reset counter removed from hot-path usage
	 * to reduce cache-line bouncing
	 */
	u8 dct_active_percent;
};

/* Core lifecycle */
void ivpu_pm_init(struct ivpu_device *vdev);
void ivpu_pm_enable(struct ivpu_device *vdev);
void ivpu_pm_disable(struct ivpu_device *vdev);

/*
 * Recovery control:
 * - prefer engine reset over full device recovery
 * - full recovery should be last resort only
 */
void ivpu_pm_trigger_recovery(struct ivpu_device *vdev, const char *reason);
void ivpu_pm_disable_recovery(struct ivpu_device *vdev);

/* Runtime PM callbacks */
int ivpu_pm_suspend_cb(struct device *dev);
int ivpu_pm_resume_cb(struct device *dev);
int ivpu_pm_runtime_suspend_cb(struct device *dev);
int ivpu_pm_runtime_resume_cb(struct device *dev);

/* Fast reset path (optimized critical path) */
void ivpu_pm_reset_prepare_cb(struct pci_dev *pdev);
void ivpu_pm_reset_done_cb(struct pci_dev *pdev);

/* Runtime power management reference counting */
int __must_check ivpu_rpm_get(struct ivpu_device *vdev);
void ivpu_rpm_put(struct ivpu_device *vdev);

/* Job timeout handling (tuned for reduced wakeups) */
void ivpu_start_job_timeout_detection(struct ivpu_device *vdev);
void ivpu_stop_job_timeout_detection(struct ivpu_device *vdev);

/* Dynamic Compute Throttling (DCT) */
int ivpu_pm_dct_init(struct ivpu_device *vdev);
int ivpu_pm_dct_enable(struct ivpu_device *vdev, u8 active_percent);
int ivpu_pm_dct_disable(struct ivpu_device *vdev);
void ivpu_pm_irq_dct_work_fn(struct work_struct *work);

#endif /* __IVPU_PM_H__ */
