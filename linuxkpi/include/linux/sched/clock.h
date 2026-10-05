/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. local_clock() and friends are gethrtime(9F). */
#ifndef _LINUX_SCHED_CLOCK_H
#define	_LINUX_SCHED_CLOCK_H
#include <linux/types.h>
static inline u64 local_clock(void) { return ((u64)gethrtime()); }
static inline u64 sched_clock(void) { return ((u64)gethrtime()); }
static inline u64 running_clock(void) { return ((u64)gethrtime()); }
#define	cpu_clock(cpu)	local_clock()
#endif
