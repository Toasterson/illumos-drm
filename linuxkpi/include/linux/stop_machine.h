/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's stop_machine(). Not supported as such (no DDI way to stop other
 * CPUs): the function runs on the calling CPU with preemption disabled
 * while other CPUs continue. i915 uses it only for the Broxton/Cherryview
 * VT-d GGTT workarounds, which gen7 and gen9.5 Core parts do not take.
 */
#ifndef _LINUX_STOP_MACHINE_H
#define	_LINUX_STOP_MACHINE_H
#include <linux/cpumask.h>
#include <linux/preempt.h>
typedef int (*cpu_stop_fn_t)(void *arg);
static inline int
stop_machine(cpu_stop_fn_t fn, void *data, const struct cpumask *cpus)
{
	int ret;

	preempt_disable();
	ret = fn(data);
	preempt_enable();
	return (ret);
}
#define	stop_machine_cpuslocked(fn, d, c)	stop_machine(fn, d, c)
#endif
