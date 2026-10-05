/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's interrupt interface over ddi_intr_*(9F) (include/linux/
 * interrupt.h, GPL-2.0; reimplemented in src/lkpi_irq.c).
 *
 * pci_alloc_irq_vectors()/pci_enable_msi() allocate MSI (or MSI-X, or
 * fixed) interrupts of the device; an "irq number" is the layer's index
 * of an allocated vector. request_irq() adds and enables the handler,
 * refusing (-EINVAL) a vector whose priority is at or above
 * ddi_intr_get_hilevel_pri(9F), because the layer's spinlocks are adaptive
 * mutexes. Handlers run in illumos interrupt-thread context.
 * synchronize_irq() waits for a running handler; disable_irq() disables
 * the vector and waits. Threaded handlers run the thread function from
 * the interrupt (no separate thread). Tasklets run on a layer worker
 * thread (thread context, src/lkpi_work.c), one at a time per tasklet, as
 * on Linux; a tasklet scheduled while disabled runs after tasklet_enable()
 * (or, if enabled behind the layer's back, at the next one-tick poll).
 */
#ifndef _LINUX_INTERRUPT_H
#define	_LINUX_INTERRUPT_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/irqreturn.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/hardirq.h>

#define	IRQF_SHARED		0x00000080
#define	IRQF_ONESHOT		0x00002000
#define	IRQF_NO_SUSPEND		0x00004000
#define	IRQF_TRIGGER_NONE	0x00000000

typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

extern int request_threaded_irq(unsigned int irq, irq_handler_t handler,
    irq_handler_t thread_fn, unsigned long flags, const char *name,
    void *dev_id);
extern const void *free_irq(unsigned int irq, void *dev_id);
extern void disable_irq(unsigned int irq);
extern void disable_irq_nosync(unsigned int irq);
extern void enable_irq(unsigned int irq);
extern void synchronize_irq(unsigned int irq);
extern void synchronize_hardirq(unsigned int irq);

static inline int
request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
    const char *name, void *dev_id)
{
	return (request_threaded_irq(irq, handler, NULL, flags, name, dev_id));
}

#define	devm_request_irq(dev, irq, h, f, n, d)	request_irq(irq, h, f, n, d)
#define	irq_set_affinity_hint(irq, m)		0
#define	disable_hardirq(irq)			(disable_irq_nosync(irq), true)

/* --- tasklets -------------------------------------------------------- */

struct tasklet_struct {
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;
	bool use_callback;
	union {
		void (*func)(unsigned long data);
		void (*callback)(struct tasklet_struct *t);
	};
	unsigned long data;
	struct work_struct lkpi_work;
};

enum {
	TASKLET_STATE_SCHED,
	TASKLET_STATE_RUN
};

#define	DECLARE_TASKLET(name, _callback) \
	struct tasklet_struct name = { .count = ATOMIC_INIT(0), \
	.callback = _callback, .use_callback = true }
#define	DECLARE_TASKLET_DISABLED(name, _callback) \
	struct tasklet_struct name = { .count = ATOMIC_INIT(1), \
	.callback = _callback, .use_callback = true }
#define	from_tasklet(var, callback_tasklet, tasklet_fieldname) \
	container_of(callback_tasklet, typeof (*var), tasklet_fieldname)

extern void tasklet_init(struct tasklet_struct *t,
    void (*func)(unsigned long), unsigned long data);
extern void tasklet_setup(struct tasklet_struct *t,
    void (*callback)(struct tasklet_struct *));
extern void tasklet_schedule(struct tasklet_struct *t);
extern void tasklet_kill(struct tasklet_struct *t);
extern void tasklet_unlock_wait(struct tasklet_struct *t);
extern void tasklet_unlock_spin_wait(struct tasklet_struct *t);
extern bool tasklet_trylock(struct tasklet_struct *t);
extern void tasklet_unlock(struct tasklet_struct *t);

#define	tasklet_hi_schedule(t)		tasklet_schedule(t)

static inline void
tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	smp_mb__after_atomic();
}

static inline void
tasklet_disable_in_atomic(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_spin_wait(t);
	smp_mb();
}

static inline void
tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
	smp_mb();
}

extern void tasklet_enable(struct tasklet_struct *t);

#endif
