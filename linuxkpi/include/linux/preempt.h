/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's preemption control over illumos kpreempt_disable() and
 * kpreempt_enable() (<sys/disp.h>), and the context tests over
 * servicing_interrupt(9F). in_atomic() cannot be known on illumos and
 * is false; in_task() is true outside interrupt handlers.
 */
#ifndef _LINUX_PREEMPT_H
#define	_LINUX_PREEMPT_H

#include <linux/types.h>

static inline void
lkpi_preempt_disable(void)
{
	kpreempt_disable();
}

static inline void
lkpi_preempt_enable(void)
{
	kpreempt_enable();
}

#define	preempt_disable()		lkpi_preempt_disable()
#define	preempt_enable()		lkpi_preempt_enable()
#define	preempt_enable_no_resched()	lkpi_preempt_enable()
#define	preempt_disable_notrace()	lkpi_preempt_disable()
#define	preempt_enable_notrace()	lkpi_preempt_enable()
#define	migrate_disable()		lkpi_preempt_disable()
#define	migrate_enable()		lkpi_preempt_enable()
#define	preempt_count()			0
#define	preemptible()			(!servicing_interrupt())
#define	in_interrupt()			(servicing_interrupt() != 0)
#define	in_irq()			(servicing_interrupt() != 0)
#define	in_hardirq()			(servicing_interrupt() != 0)
#define	in_nmi()			0
#define	in_task()			(servicing_interrupt() == 0)
#define	in_atomic()			0
#define	irq_count()			0
#define	hardirq_count()			0
#define	softirq_count()			0

#endif
