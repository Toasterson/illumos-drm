/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's irq_work (include/linux/irq_work.h, GPL-2.0; reimplemented):
 * a callback run soon from another context. The layer runs it on its own
 * high-priority worker thread (thread context, not an interrupt), queued
 * without allocation from any context.
 */
#ifndef _LINUX_IRQ_WORK_H
#define	_LINUX_IRQ_WORK_H

#include <linux/workqueue.h>

struct irq_work {
	struct work_struct work;
	void (*func)(struct irq_work *);
	unsigned int flags;
};

#define	IRQ_WORK_INIT(f)	{ .func = (f) }
#define	IRQ_WORK_INIT_HARD(f)	{ .func = (f) }
#define	IRQ_WORK_INIT_LAZY(f)	{ .func = (f) }
#define	DEFINE_IRQ_WORK(name, f)	struct irq_work name = IRQ_WORK_INIT(f)

extern void init_irq_work(struct irq_work *w, void (*func)(struct irq_work *));
extern bool irq_work_queue(struct irq_work *w);
extern void irq_work_sync(struct irq_work *w);
#define	irq_work_queue_on(w, cpu)	irq_work_queue(w)
#define	irq_work_is_pending(w)		work_pending(&(w)->work)
#define	irq_work_is_busy(w)		work_busy(&(w)->work)
#define	irq_work_run()			((void)0)

#endif
