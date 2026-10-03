/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's power-management callbacks. The layer does not suspend or
 * resume devices (illumos DDI_SUSPEND/DDI_RESUME are refused by the
 * layer's attach/detach glue), so struct dev_pm_ops tables are kept but
 * never called; struct dev_pm_info holds what drivers read.
 */
#ifndef _LINUX_PM_H
#define	_LINUX_PM_H

#include <linux/types.h>

struct device;

typedef struct pm_message {
	int event;
} pm_message_t;

#define	PM_EVENT_ON		0x0000
#define	PM_EVENT_FREEZE		0x0001
#define	PM_EVENT_SUSPEND	0x0002
#define	PM_EVENT_HIBERNATE	0x0004
#define	PM_EVENT_QUIESCE	0x0008
#define	PMSG_SUSPEND		((struct pm_message){ .event = PM_EVENT_SUSPEND, })
#define	PMSG_FREEZE		((struct pm_message){ .event = PM_EVENT_FREEZE, })
#define	PMSG_ON			((struct pm_message){ .event = PM_EVENT_ON, })

struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
	int (*freeze_noirq)(struct device *dev);
	int (*thaw_noirq)(struct device *dev);
	int (*poweroff_noirq)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};

struct dev_pm_info {
	atomic_t usage_count;
	bool is_suspended;
	bool runtime_auto;
	bool no_callbacks;
	bool ignore_children;
	int runtime_error;
	int autosuspend_delay;
};

#define	SET_SYSTEM_SLEEP_PM_OPS(s, r)
#define	SET_RUNTIME_PM_OPS(s, r, i)
#define	SYSTEM_SLEEP_PM_OPS(s, r)
#define	RUNTIME_PM_OPS(s, r, i)
#define	pm_sleep_ptr(p)		NULL
#define	pm_ptr(p)		NULL
#define	pm_suspend_via_firmware()	false
#define	pm_resume_via_firmware()	false
#define	pm_suspend_target_state	0
#define	PM_SUSPEND_ON		0
#define	PM_SUSPEND_TO_IDLE	1
#define	PM_SUSPEND_MEM		3

#endif
