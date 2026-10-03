/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's processor identity and cross-calls (include/linux/smp.h,
 * GPL-2.0). smp_processor_id() is the current CPU's cpu_id (meaningful
 * only with preemption disabled, as on Linux). on_each_cpu() runs the
 * function once on the calling CPU only (no cross-calls through the DDI);
 * drivers use it for cache maintenance, which the layer does with wbinvd
 * itself (<asm/cacheflush.h>).
 */
#ifndef _LINUX_SMP_H
#define	_LINUX_SMP_H

#include <linux/types.h>
#include <linux/cpumask.h>

#define	smp_processor_id()		((int)CPU->cpu_id)
#define	raw_smp_processor_id()		((int)CPU->cpu_id)
#define	get_cpu()			({ preempt_disable(); smp_processor_id(); })
#define	put_cpu()			preempt_enable()
#define	on_each_cpu(fn, info, wait)	((fn)(info))
#define	smp_call_function(fn, info, w)	((void)(fn), 0)
#define	smp_call_function_single(cpu, fn, info, wait)	({ (fn)(info); 0; })
#define	wbinvd_on_all_cpus()		lkpi_wbinvd_all()

extern void lkpi_wbinvd_all(void);

#endif
