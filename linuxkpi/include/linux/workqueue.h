/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's workqueues (include/linux/workqueue.h, GPL-2.0; reimplemented
 * in src/lkpi_work.c).
 *
 * A workqueue is a list of pending work items served by worker threads
 * of a ddi_taskq(9F) whose tasks loop on the queue (one thread for
 * ordered queues, otherwise max_active, by default and at most a number
 * that grows with the CPU count; see src/lkpi_work.c). queue_work()
 * only links the item and signals a worker, so it works in interrupt
 * handlers and never allocates. As on Linux, a pending item is queued at
 * most once, an item never runs on two workers at the same time, and an
 * item may requeue itself. flush_work() waits until the item is neither
 * pending nor running; flush_workqueue() and drain_workqueue() wait until
 * the queue is idle. Delayed work arms a layer timer that queues the item.
 * system_wq, system_highpri_wq, system_long_wq, system_unbound_wq,
 * system_freezable_wq, system_power_efficient_wq and
 * system_freezable_power_efficient_wq are created when misc/linuxkpi
 * loads; system_percpu_wq and system_dfl_wq (Linux's newer names) are
 * system_wq and system_unbound_wq. Flushing a queue from one of its own
 * items, or an item from itself, warns instead of deadlocking.
 * disable_work*() cancel an item and keep it from being queued (queueing
 * returns false) until as many enable_work*() calls; destroy_workqueue()
 * disarms (and reports) delayed items still armed for the queue.
 */
#ifndef _LINUX_WORKQUEUE_H
#define	_LINUX_WORKQUEUE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/timer.h>
#include <linux/workqueue_types.h>
#include <linux/completion.h>
#include <linux/rcupdate.h>
#include <linux/cpumask.h>

#define	WORK_STRUCT_PENDING_BIT	0
#define	WORK_STRUCT_RUNNING_BIT	1
#define	WORK_STRUCT_PENDING	(1UL << WORK_STRUCT_PENDING_BIT)
#define	WORK_CPU_UNBOUND	(NR_CPUS)

enum {
	WQ_UNBOUND = 1 << 1,
	WQ_FREEZABLE = 1 << 2,
	WQ_MEM_RECLAIM = 1 << 3,
	WQ_HIGHPRI = 1 << 4,
	WQ_CPU_INTENSIVE = 1 << 5,
	WQ_SYSFS = 1 << 6,
	WQ_POWER_EFFICIENT = 1 << 7,
	WQ_PERCPU = 1 << 8,
	__WQ_ORDERED = 1 << 17,
	WQ_MAX_ACTIVE = 512,
	WQ_UNBOUND_MAX_ACTIVE = WQ_MAX_ACTIVE,
	WQ_DFL_ACTIVE = WQ_MAX_ACTIVE / 2
};

struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
	struct workqueue_struct *wq;
	int cpu;
	struct list_head lkpi_dlink;	/* wq's armed items, while armed */
};

struct rcu_work {
	struct work_struct work;
	struct rcu_head rcu;
	struct workqueue_struct *wq;
};

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_percpu_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
extern struct workqueue_struct *system_dfl_wq;
extern struct workqueue_struct *system_freezable_wq;
extern struct workqueue_struct *system_power_efficient_wq;
extern struct workqueue_struct *system_freezable_power_efficient_wq;

extern struct workqueue_struct *lkpi_alloc_workqueue(const char *name,
    unsigned int flags, int max_active);
extern struct workqueue_struct *alloc_workqueue(const char *fmt,
    unsigned int flags, int max_active, ...) __printf(1, 4);
extern void destroy_workqueue(struct workqueue_struct *wq);
extern bool queue_work(struct workqueue_struct *wq, struct work_struct *w);
extern bool queue_delayed_work(struct workqueue_struct *wq,
    struct delayed_work *dw, unsigned long delay);
extern bool mod_delayed_work(struct workqueue_struct *wq,
    struct delayed_work *dw, unsigned long delay);
extern bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rw);
extern bool flush_work(struct work_struct *w);
extern bool flush_delayed_work(struct delayed_work *dw);
extern bool cancel_work(struct work_struct *w);
extern bool cancel_work_sync(struct work_struct *w);
extern bool cancel_delayed_work(struct delayed_work *dw);
extern bool cancel_delayed_work_sync(struct delayed_work *dw);
extern void flush_workqueue(struct workqueue_struct *wq);
extern void drain_workqueue(struct workqueue_struct *wq);
extern bool work_busy(struct work_struct *w);
extern struct work_struct *current_work(void);
extern void lkpi_init_work(struct work_struct *w, work_func_t func);
extern void lkpi_init_delayed_work(struct delayed_work *dw, work_func_t func);
extern bool disable_work(struct work_struct *w);
extern bool disable_work_sync(struct work_struct *w);
extern bool enable_work(struct work_struct *w);
extern bool disable_delayed_work(struct delayed_work *dw);
extern bool disable_delayed_work_sync(struct delayed_work *dw);
extern bool enable_delayed_work(struct delayed_work *dw);

#define	alloc_ordered_workqueue(fmt, flags, args...) \
	alloc_workqueue(fmt, __WQ_ORDERED | (flags), 1, ##args)
#define	create_workqueue(name)	alloc_workqueue("%s", WQ_MEM_RECLAIM, 1, (name))
#define	create_singlethread_workqueue(name) \
	alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM, name)
#define	INIT_WORK(w, f)			lkpi_init_work((w), (f))
#define	INIT_WORK_ONSTACK(w, f)		lkpi_init_work((w), (f))
#define	__INIT_WORK(w, f, onstack)	lkpi_init_work((w), (f))
#define	INIT_DELAYED_WORK(dw, f)	lkpi_init_delayed_work((dw), (f))
#define	INIT_DELAYED_WORK_ONSTACK(dw, f)	lkpi_init_delayed_work((dw), (f))
#define	INIT_DEFERRABLE_WORK(dw, f)	lkpi_init_delayed_work((dw), (f))
#define	INIT_RCU_WORK(rw, f)		lkpi_init_work(&(rw)->work, (f))
#define	destroy_work_on_stack(w)	((void)(w))
#define	destroy_delayed_work_on_stack(dw)	((void)(dw))
#define	__WORK_INITIALIZER(n, f) { \
	.data = ATOMIC_LONG_INIT(0), \
	.entry = { &(n).entry, &(n).entry }, \
	.func = (f) }
#define	DECLARE_WORK(n, f)	struct work_struct n = __WORK_INITIALIZER(n, f)
#define	DECLARE_DELAYED_WORK(n, f) \
	struct delayed_work n = { .work = __WORK_INITIALIZER((n).work, (f)) }
#define	to_delayed_work(w)	container_of(w, struct delayed_work, work)
#define	to_rcu_work(w)		container_of(w, struct rcu_work, work)
#define	work_data_bits(w)	((unsigned long *)(&(w)->data.counter))
#define	work_pending(w)		test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w))
#define	delayed_work_pending(dw)	work_pending(&(dw)->work)
#define	queue_work_on(cpu, wq, w)	queue_work((wq), (w))
#define	queue_work_node(n, wq, w)	queue_work((wq), (w))
#define	queue_delayed_work_on(cpu, wq, dw, d)	queue_delayed_work(wq, dw, d)
#define	mod_delayed_work_on(cpu, wq, dw, d)	mod_delayed_work(wq, dw, d)
#define	schedule_work(w)		queue_work(system_wq, (w))
#define	schedule_work_on(cpu, w)	queue_work(system_wq, (w))
#define	schedule_delayed_work(dw, d)	queue_delayed_work(system_wq, (dw), (d))
#define	schedule_delayed_work_on(c, dw, d)	queue_delayed_work(system_wq, (dw), (d))
#define	flush_scheduled_work()		flush_workqueue(system_wq)
#define	workqueue_set_max_active(wq, n)	((void)(wq), (void)(n))
#define	current_is_workqueue_rescuer()	false

#endif
