/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's preemption control over illumos kpreempt_disable() and
 * kpreempt_enable() (<sys/disp.h>), and the context tests over
 * servicing_interrupt(9F). in_atomic() cannot be known on illumos and
 * is false; in_task() is true outside interrupt handlers. The preemption
 * calls are out of line in misc/linuxkpi (src/lkpi_sched.c), so drivers
 * do not bind to kpreempt(), which is not part of the DDI.
 */
#ifndef _LINUX_PREEMPT_H
#define	_LINUX_PREEMPT_H

#include <linux/types.h>

extern void lkpi_preempt_disable(void);
extern void lkpi_preempt_enable(void);
/* servicing_interrupt(9F), out of line: the DDI mapfile lacks it. */
extern int lkpi_in_interrupt(void);

#define	preempt_disable()		lkpi_preempt_disable()
#define	preempt_enable()		lkpi_preempt_enable()
#define	preempt_enable_no_resched()	lkpi_preempt_enable()
#define	preempt_disable_notrace()	lkpi_preempt_disable()
#define	preempt_enable_notrace()	lkpi_preempt_enable()
#define	migrate_disable()		lkpi_preempt_disable()
#define	migrate_enable()		lkpi_preempt_enable()
#define	preempt_count()			0
#define	preemptible()			(!lkpi_in_interrupt())
#define	in_interrupt()			(lkpi_in_interrupt() != 0)
#define	in_irq()			(lkpi_in_interrupt() != 0)
#define	in_hardirq()			(lkpi_in_interrupt() != 0)
#define	in_nmi()			0
#define	in_task()			(lkpi_in_interrupt() == 0)
#define	in_atomic()			0
#define	irq_count()			0
#define	hardirq_count()			0
#define	softirq_count()			0

#endif
