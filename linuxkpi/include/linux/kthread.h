/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kernel threads (src/lkpi_kthread.c), each the one thread of its
 * own ddi_taskq(9F) (thread_create() is not part of the DDI). The thread
 * exists from kthread_create() on but calls the function only after the
 * first wake_up_process(); kthread_run() starts it at once;
 * kthread_stop() sets the stop flag, wakes the thread and waits for it to
 * return; kthread_should_stop() is the flag. A kthread whose function
 * returns by itself keeps its (idle) thread until kthread_stop() or
 * module unload. kthread_park() is not supported (returns -ENOSYS).
 *
 * kthread workers (Linux's kthread_worker API): a worker is one kthread
 * that runs the works queued on it in order.
 * kthread_create_worker(flags, fmt, ...) / kthread_run_worker(...)
 *	create the worker and its thread (run: started at once; create: when
 *	the caller wakes worker->task); ERR_PTR(-ENOMEM) on failure.
 * kthread_init_work(work, fn)
 *	initialises a work (not queued).
 * kthread_queue_work(worker, work)
 *	queues the work unless it is already queued; true if it queued it.
 *	Any context (takes a spin lock).
 * kthread_flush_work(work)
 *	waits until the work, if queued or running, has run.
 * kthread_flush_worker(worker)
 *	waits until every work queued before the call has run.
 * kthread_cancel_work_sync(work)
 *	dequeues the work and waits for a running instance; true if it was
 *	pending. The work may not be requeued meanwhile by the caller.
 * kthread_destroy_worker(worker)
 *	flushes the worker, stops its thread and frees it; the worker must
 *	have no works queued afterwards.
 */
#ifndef _LINUX_KTHREAD_H
#define	_LINUX_KTHREAD_H
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
extern struct task_struct *lkpi_kthread_create(int (*fn)(void *data), void *data, const char *namefmt, ...) __printf(3, 4);
extern int kthread_stop(struct task_struct *k);
extern bool kthread_should_stop(void);
extern bool kthread_should_park(void);
extern void kthread_parkme(void);
extern int kthread_park(struct task_struct *k);
extern void kthread_unpark(struct task_struct *k);
#define	kthread_create(fn, data, fmt, ...)	lkpi_kthread_create(fn, data, fmt, ##__VA_ARGS__)
#define	kthread_run(fn, data, fmt, ...) ({ \
	struct task_struct *__k = lkpi_kthread_create(fn, data, fmt, ##__VA_ARGS__); \
	if (!IS_ERR(__k)) wake_up_process(__k); \
	__k; })
struct kthread_work;
struct kthread_worker;
typedef void (*kthread_work_func_t)(struct kthread_work *work);

struct kthread_worker {
	unsigned int flags;
	spinlock_t lock;
	struct list_head work_list;
	struct list_head delayed_work_list;
	struct task_struct *task;
	struct kthread_work *current_work;
};

struct kthread_work {
	struct list_head node;
	kthread_work_func_t func;
	struct kthread_worker *worker;
	int canceling;
};

#define	KTHREAD_WORK_INIT(work, fn)	{ \
	.node = LIST_HEAD_INIT((work).node), .func = (fn) }

static inline void
kthread_init_work(struct kthread_work *work, kthread_work_func_t fn)
{
	INIT_LIST_HEAD(&work->node);
	work->func = fn;
	work->worker = NULL;
	work->canceling = 0;
}

extern struct kthread_worker *kthread_create_worker(unsigned int flags,
    const char *namefmt, ...) __printf(2, 3);
extern struct kthread_worker *kthread_run_worker(unsigned int flags,
    const char *namefmt, ...) __printf(2, 3);
extern bool kthread_queue_work(struct kthread_worker *worker,
    struct kthread_work *work);
extern void kthread_flush_work(struct kthread_work *work);
extern void kthread_flush_worker(struct kthread_worker *worker);
extern bool kthread_cancel_work_sync(struct kthread_work *work);
extern void kthread_destroy_worker(struct kthread_worker *worker);

#define	kthread_bind(k, cpu)	((void)(k))
#define	set_freezable()		((void)0)
#define	try_to_freeze()		false
#define	freezing(t)		false
#endif
