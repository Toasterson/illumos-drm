/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's MMU notifiers. Not supported on illumos (no address-space
 * change callbacks); the configuration has CONFIG_MMU_NOTIFIER off, so
 * i915 userptr is unavailable.
 */
#ifndef _LINUX_MMU_NOTIFIER_H
#define	_LINUX_MMU_NOTIFIER_H
#include <linux/mm_types.h>
struct mmu_notifier;
struct mmu_interval_notifier {
	unsigned long notifier_seq;
};
struct mmu_notifier_range {
	unsigned long start;
	unsigned long end;
};
#endif
