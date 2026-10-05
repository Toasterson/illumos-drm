/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's perf events. Not supported (CONFIG_PERF_EVENTS is off; i915's
 * PMU is not built).
 */
#ifndef _LINUX_PERF_EVENT_H
#define	_LINUX_PERF_EVENT_H
#include <linux/types.h>
#include <linux/sysfs.h>
struct perf_event;
/* Only the size matters: i915's struct i915_pmu embeds one (i915_pmu.c is not built). */
struct pmu {
	void *lkpi_unused[8];
};
#endif
