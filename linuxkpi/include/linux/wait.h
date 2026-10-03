/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's wait queues (include/linux/wait.h, GPL-2.0; reimplemented in
 * src/lkpi_wait.c on the layer's tasks, <linux/sched.h>). The structure
 * layout and the wake-function protocol are Linux's, because drivers
 * (i915_sw_fence) add entries with their own wake functions and walk
 * wq_head->head under wq_head->lock.
 */
#ifndef _LINUX_WAIT_H
#define	_LINUX_WAIT_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

typedef struct wait_queue_entry wait_queue_entry_t;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry,
    unsigned int mode, int flags, void *key);

#define	WQ_FLAG_EXCLUSIVE	0x01
#define	WQ_FLAG_WOKEN		0x02
#define	WQ_FLAG_CUSTOM		0x04
#define	WQ_FLAG_DONE		0x08
#define	WQ_FLAG_PRIORITY	0x10

struct wait_queue_entry {
	unsigned int flags;
	void *private;
	wait_queue_func_t func;
	struct list_head entry;
};

struct wait_queue_head {
	spinlock_t lock;
	struct list_head head;
};
typedef struct wait_queue_head wait_queue_head_t;

extern int default_wake_function(struct wait_queue_entry *wq_entry,
    unsigned int mode, int flags, void *key);
extern int autoremove_wake_function(struct wait_queue_entry *wq_entry,
    unsigned int mode, int sync, void *key);
extern int woken_wake_function(struct wait_queue_entry *wq_entry,
    unsigned int mode, int sync, void *key);

#define	__WAIT_QUEUE_HEAD_INITIALIZER(name) \
	{ .lock = { }, .head = LIST_HEAD_INIT((name).head) }
#define	DECLARE_WAIT_QUEUE_HEAD(name) \
	struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)
#define	__WAITQUEUE_INITIALIZER(name, tsk) \
	{ .private = (tsk), .func = default_wake_function, \
	.entry = { NULL, NULL } }
#define	DECLARE_WAITQUEUE(name, tsk) \
	struct wait_queue_entry name = __WAITQUEUE_INITIALIZER(name, tsk)
#define	DEFINE_WAIT_FUNC(name, function) \
	struct wait_queue_entry name = { .private = current, \
	.func = (function), .entry = LIST_HEAD_INIT((name).entry) }
#define	DEFINE_WAIT(name)	DEFINE_WAIT_FUNC(name, autoremove_wake_function)

static inline void
lkpi_init_waitqueue_head(struct wait_queue_head *wq)
{
	spin_lock_init(&wq->lock);
	INIT_LIST_HEAD(&wq->head);
}
#define	init_waitqueue_head(wq)		lkpi_init_waitqueue_head(wq)
#define	__init_waitqueue_head(wq, n, k)	lkpi_init_waitqueue_head(wq)

static inline void
init_waitqueue_entry(struct wait_queue_entry *e, struct task_struct *p)
{
	e->flags = 0;
	e->private = p;
	e->func = default_wake_function;
}

static inline void
init_waitqueue_func_entry(struct wait_queue_entry *e, wait_queue_func_t func)
{
	e->flags = 0;
	e->private = NULL;
	e->func = func;
}

static inline void
init_wait_entry(struct wait_queue_entry *e, int flags)
{
	e->flags = flags;
	e->private = current;
	e->func = autoremove_wake_function;
	INIT_LIST_HEAD(&e->entry);
}
#define	init_wait(w)	init_wait_entry(w, 0)

static inline int
waitqueue_active(struct wait_queue_head *wq)
{
	return (!list_empty(&wq->head));
}

static inline bool
wq_has_sleeper(struct wait_queue_head *wq)
{
	smp_mb();
	return (waitqueue_active(wq));
}

static inline void
__add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	list_add(&e->entry, &wq->head);
}

static inline void
__add_wait_queue_entry_tail(struct wait_queue_head *wq,
    struct wait_queue_entry *e)
{
	list_add_tail(&e->entry, &wq->head);
}

static inline void
__remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	list_del(&e->entry);
}

extern void add_wait_queue(struct wait_queue_head *wq,
    struct wait_queue_entry *e);
extern void add_wait_queue_exclusive(struct wait_queue_head *wq,
    struct wait_queue_entry *e);
extern void remove_wait_queue(struct wait_queue_head *wq,
    struct wait_queue_entry *e);
extern void prepare_to_wait(struct wait_queue_head *wq,
    struct wait_queue_entry *e, int state);
extern bool prepare_to_wait_exclusive(struct wait_queue_head *wq,
    struct wait_queue_entry *e, int state);
extern long prepare_to_wait_event(struct wait_queue_head *wq,
    struct wait_queue_entry *e, int state);
extern void finish_wait(struct wait_queue_head *wq,
    struct wait_queue_entry *e);
extern long wait_woken(struct wait_queue_entry *e, unsigned int mode,
    long timeout);
extern void __wake_up(struct wait_queue_head *wq, unsigned int mode,
    int nr, void *key);
extern void __wake_up_locked(struct wait_queue_head *wq, unsigned int mode,
    int nr);
extern void __wake_up_locked_key(struct wait_queue_head *wq,
    unsigned int mode, void *key);

#define	wake_up(x)			__wake_up(x, TASK_NORMAL, 1, NULL)
#define	wake_up_nr(x, nr)		__wake_up(x, TASK_NORMAL, nr, NULL)
#define	wake_up_all(x)			__wake_up(x, TASK_NORMAL, 0, NULL)
#define	wake_up_locked(x)		__wake_up_locked((x), TASK_NORMAL, 1)
#define	wake_up_all_locked(x)		__wake_up_locked((x), TASK_NORMAL, 0)
#define	wake_up_interruptible(x)	__wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define	wake_up_interruptible_nr(x, n)	__wake_up(x, TASK_INTERRUPTIBLE, n, NULL)
#define	wake_up_interruptible_all(x)	__wake_up(x, TASK_INTERRUPTIBLE, 0, NULL)
#define	wake_up_interruptible_sync(x)	__wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define	wake_up_poll(x, m)		__wake_up(x, TASK_NORMAL, 1, (void *)(uintptr_t)(m))
#define	wake_up_interruptible_poll(x, m) \
	__wake_up(x, TASK_INTERRUPTIBLE, 1, (void *)(uintptr_t)(m))

/*
 * The wait_event family. The condition is evaluated in the waiting
 * thread after it queued itself, so a wake-up between the test and the
 * sleep is not lost (the task's state is RUNNING again then).
 */
#define	___wait_event(wq, condition, state, exclusive, ret, cmd) ({ \
	__label__ __out; \
	struct wait_queue_entry __wq_entry; \
	long __ret = (ret); \
	init_wait_entry(&__wq_entry, (exclusive) ? WQ_FLAG_EXCLUSIVE : 0); \
	for (;;) { \
		long __int = prepare_to_wait_event(&(wq), &__wq_entry, (state)); \
		if (condition) \
			break; \
		if (__int != 0) { \
			__ret = __int; \
			goto __out; \
		} \
		cmd; \
	} \
	finish_wait(&(wq), &__wq_entry); \
__out: \
	__ret; })

#define	wait_event(wq, condition) do { \
	if (condition) \
		break; \
	(void)___wait_event(wq, condition, TASK_UNINTERRUPTIBLE, 0, 0, schedule()); \
	} while (0)
#define	wait_event_freezable(wq, c)	wait_event_interruptible(wq, c)
#define	wait_event_interruptible(wq, condition) ({ \
	int __r = 0; \
	if (!(condition)) \
		__r = (int)___wait_event(wq, condition, TASK_INTERRUPTIBLE, 0, 0, schedule()); \
	__r; })
#define	wait_event_killable(wq, condition) ({ \
	int __r = 0; \
	if (!(condition)) \
		__r = (int)___wait_event(wq, condition, TASK_KILLABLE, 0, 0, schedule()); \
	__r; })
#define	___wait_cond_timeout(condition) ({ \
	bool __cond = (condition); \
	if (__cond && !__ret) \
		__ret = 1; \
	__cond || !__ret; })
#define	wait_event_timeout(wq, condition, timeout) ({ \
	long __ret = (timeout); \
	if (!___wait_cond_timeout(condition)) \
		__ret = ___wait_event(wq, ___wait_cond_timeout(condition), \
		    TASK_UNINTERRUPTIBLE, 0, timeout, \
		    __ret = schedule_timeout(__ret)); \
	__ret; })
#define	wait_event_interruptible_timeout(wq, condition, timeout) ({ \
	long __ret = (timeout); \
	if (!___wait_cond_timeout(condition)) \
		__ret = ___wait_event(wq, ___wait_cond_timeout(condition), \
		    TASK_INTERRUPTIBLE, 0, timeout, \
		    __ret = schedule_timeout(__ret)); \
	__ret; })
#define	wait_event_killable_timeout(wq, c, t) \
	wait_event_interruptible_timeout(wq, c, t)
#define	wait_event_interruptible_locked(wq, condition) ({ \
	int __r = 0; \
	while (!(condition)) { \
		spin_unlock(&(wq).lock); \
		__r = wait_event_interruptible(wq, condition); \
		spin_lock(&(wq).lock); \
		if (__r) break; \
	} \
	__r; })
#define	wait_event_lock_irq(wq, condition, lock) do { \
	while (!(condition)) { \
		spin_unlock_irq(&(lock)); \
		wait_event_timeout(wq, condition, 1); \
		spin_lock_irq(&(lock)); \
	} } while (0)

#endif
