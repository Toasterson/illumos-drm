/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's CPU masks, reduced (include/linux/cpumask.h, GPL-2.0). The
 * number of CPUs is illumos' ncpus; iteration covers 0 .. ncpus - 1.
 */
#ifndef _LINUX_CPUMASK_H
#define	_LINUX_CPUMASK_H

#include <linux/types.h>
#include <linux/bitops.h>

struct cpumask {
	unsigned long bits[NR_CPUS / BITS_PER_LONG];
};
typedef struct cpumask cpumask_t;
typedef struct cpumask cpumask_var_t[1];

#define	nr_cpu_ids		((unsigned int)ncpus)
#define	num_online_cpus()	((unsigned int)ncpus)
#define	num_possible_cpus()	((unsigned int)ncpus)
#define	num_present_cpus()	((unsigned int)ncpus)
#define	cpu_online(cpu)		((unsigned int)(cpu) < (unsigned int)ncpus)
#define	cpu_possible(cpu)	cpu_online(cpu)
#define	for_each_possible_cpu(cpu) \
	for ((cpu) = 0; (cpu) < ncpus; (cpu)++)
#define	for_each_online_cpu(cpu)	for_each_possible_cpu(cpu)
#define	for_each_present_cpu(cpu)	for_each_possible_cpu(cpu)
#define	cpumask_bits(m)		((m)->bits)
#define	cpumask_set_cpu(c, m)	__set_bit((c), (m)->bits)
#define	cpumask_clear_cpu(c, m)	__clear_bit((c), (m)->bits)
#define	cpumask_test_cpu(c, m)	test_bit((c), (m)->bits)
#define	cpumask_clear(m)	memset((m)->bits, 0, sizeof ((m)->bits))
#define	cpumask_first(m)	((unsigned int)find_first_bit((m)->bits, NR_CPUS))
#define	cpumask_weight(m)	((unsigned int)bitmap_weight((m)->bits, NR_CPUS))
#define	zalloc_cpumask_var(m, g)	(memset(*(m), 0, sizeof (**(m))), true)
#define	alloc_cpumask_var(m, g)		true
#define	free_cpumask_var(m)		((void)(m))
#define	cpu_online_mask		((const struct cpumask *)NULL)

#endif
