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
 */
#ifndef _LINUX_KTHREAD_H
#define	_LINUX_KTHREAD_H
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/completion.h>
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
#define	kthread_bind(k, cpu)	((void)(k))
#define	set_freezable()		((void)0)
#define	try_to_freeze()		false
#define	freezing(t)		false
#endif
