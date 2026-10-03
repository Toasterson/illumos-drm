/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct timer_list. `arm` is the layer's record of the pending
 * illumos callout (src/lkpi_timer.c); `expires` is in jiffies.
 */
#ifndef _LINUX_TIMER_TYPES_H
#define	_LINUX_TIMER_TYPES_H

#include <linux/types.h>

struct lkpi_timer_arm;

struct timer_list {
	struct hlist_node entry;
	unsigned long expires;
	void (*function)(struct timer_list *);
	u32 flags;
	struct lkpi_timer_arm *arm;
};

#endif
