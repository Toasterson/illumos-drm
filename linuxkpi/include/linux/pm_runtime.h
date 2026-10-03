/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's runtime power management. Not supported on illumos: devices are
 * always active. pm_runtime_get*() and pm_runtime_resume*() return 0 (or
 * 1 for the "already active" forms, as Linux does for an active device),
 * the put and autosuspend calls do nothing, pm_runtime_active() is true
 * and pm_runtime_suspended() false. The usage count is kept, so a driver
 * that reads it sees its own gets and puts.
 */
#ifndef _LINUX_PM_RUNTIME_H
#define	_LINUX_PM_RUNTIME_H

#include <linux/device.h>
#include <linux/pm.h>

#define	RPM_GET_PUT	0x04

static inline int
pm_runtime_get_sync(struct device *dev)
{
	atomic_inc(&dev->power.usage_count);
	return (1);
}

static inline int
pm_runtime_resume_and_get(struct device *dev)
{
	atomic_inc(&dev->power.usage_count);
	return (0);
}

static inline int
pm_runtime_get_if_in_use(struct device *dev)
{
	return (atomic_inc_not_zero(&dev->power.usage_count) ? 1 : 0);
}

static inline int
pm_runtime_get_if_active(struct device *dev)
{
	atomic_inc(&dev->power.usage_count);
	return (1);
}

static inline void
pm_runtime_get_noresume(struct device *dev)
{
	atomic_inc(&dev->power.usage_count);
}

static inline int
pm_runtime_put(struct device *dev)
{
	atomic_dec(&dev->power.usage_count);
	return (0);
}

#define	pm_runtime_get(dev)			pm_runtime_get_sync(dev)
#define	pm_runtime_put_sync(dev)		pm_runtime_put(dev)
#define	pm_runtime_put_autosuspend(dev)		pm_runtime_put(dev)
#define	pm_runtime_put_sync_autosuspend(dev)	pm_runtime_put(dev)
#define	pm_runtime_put_noidle(dev)		((void)pm_runtime_put(dev))
#define	pm_runtime_put_sync_suspend(dev)	pm_runtime_put(dev)
#define	pm_runtime_resume(dev)			((void)(dev), 0)
#define	pm_runtime_suspend(dev)			((void)(dev), 0)
#define	pm_runtime_idle(dev)			((void)(dev), 0)
#define	pm_runtime_mark_last_busy(dev)		((void)(dev))
#define	pm_runtime_set_autosuspend_delay(dev, d)	((void)(dev), (void)(d))
#define	pm_runtime_use_autosuspend(dev)		((void)(dev))
#define	pm_runtime_dont_use_autosuspend(dev)	((void)(dev))
#define	pm_runtime_allow(dev)			((void)(dev))
#define	pm_runtime_forbid(dev)			((void)(dev))
#define	pm_runtime_enable(dev)			((void)(dev))
#define	pm_runtime_disable(dev)			((void)(dev))
#define	pm_runtime_set_active(dev)		((void)(dev), 0)
#define	pm_runtime_set_suspended(dev)		((void)(dev), 0)
#define	pm_runtime_irq_safe(dev)		((void)(dev))
#define	pm_runtime_no_callbacks(dev)		((void)(dev))
#define	pm_runtime_active(dev)			((void)(dev), true)
#define	pm_runtime_suspended(dev)		((void)(dev), false)
#define	pm_runtime_status_suspended(dev)	((void)(dev), false)
#define	pm_runtime_enabled(dev)			((void)(dev), false)
#define	pm_runtime_get_noresume_lkpi(dev)	pm_runtime_get_noresume(dev)
#define	pm_runtime_barrier(dev)			((void)(dev))
#define	pm_suspend_ignore_children(dev, e)	((void)(dev), (void)(e))
#define	pm_runtime_autosuspend_expiration(dev)	0
#define	pm_runtime_force_suspend(dev)		0
#define	pm_runtime_force_resume(dev)		0
#define	device_set_wakeup_capable(dev, c)	((void)(dev))
#define	device_wakeup_enable(dev)		0

#endif
