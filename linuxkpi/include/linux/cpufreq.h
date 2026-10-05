/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's CPU frequency interface, reduced to the maximum frequency query
 * i915 uses for its ring/LLC frequency table: cpufreq_quick_get_max() and
 * cpufreq_get_hw_max_freq() return 0 (unknown), which i915 handles by
 * using the TSC frequency.
 */
#ifndef _LINUX_CPUFREQ_H
#define	_LINUX_CPUFREQ_H
#include <linux/types.h>
#define	cpufreq_quick_get_max(cpu)	0U
#define	cpufreq_get_hw_max_freq(cpu)	0U
#define	cpufreq_quick_get(cpu)		0U
struct cpufreq_cpuinfo {
	unsigned int max_freq;
	unsigned int min_freq;
	unsigned int transition_latency;
};
struct cpufreq_policy {
	struct cpufreq_cpuinfo cpuinfo;
	unsigned int min, max, cur;
};
/* No cpufreq driver on illumos: no policy. */
#define	cpufreq_cpu_get(cpu)	((struct cpufreq_policy *)NULL)
#define	cpufreq_cpu_put(p)	((void)(p))
#endif
