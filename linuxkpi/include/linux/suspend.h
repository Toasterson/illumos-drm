/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's system sleep states. Not supported: the layer never suspends a
 * device; pm_suspend_target_state is 0 (PM_SUSPEND_ON).
 */
#ifndef _LINUX_SUSPEND_H
#define	_LINUX_SUSPEND_H
#include <linux/pm.h>
#include <linux/notifier.h>
typedef int suspend_state_t;
#define	register_pm_notifier(nb)	0
#define	unregister_pm_notifier(nb)	0
#define	pm_suspend_default_s2idle()	false
#endif
