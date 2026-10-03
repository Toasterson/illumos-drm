/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's notifier chains (include/linux/notifier.h, GPL-2.0;
 * reimplemented in src/lkpi_misc.c): blocking chains under a rw
 * semaphore, atomic chains under a spinlock, called in priority order.
 */
#ifndef _LINUX_NOTIFIER_H
#define	_LINUX_NOTIFIER_H
#include <linux/types.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb, unsigned long action, void *data);
struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block *next;
	int priority;
};
struct blocking_notifier_head {
	struct rw_semaphore rwsem;
	struct notifier_block *head;
};
struct atomic_notifier_head {
	spinlock_t lock;
	struct notifier_block *head;
};
#define	NOTIFY_DONE		0x0000
#define	NOTIFY_OK		0x0001
#define	NOTIFY_STOP_MASK	0x8000
#define	NOTIFY_BAD		(NOTIFY_STOP_MASK | 0x0002)
#define	NOTIFY_STOP		(NOTIFY_OK | NOTIFY_STOP_MASK)
#define	BLOCKING_NOTIFIER_HEAD(name)	struct blocking_notifier_head name = { }
#define	ATOMIC_NOTIFIER_HEAD(name)	struct atomic_notifier_head name = { }
#define	BLOCKING_INIT_NOTIFIER_HEAD(h)	do { init_rwsem(&(h)->rwsem); (h)->head = NULL; } while (0)
#define	ATOMIC_INIT_NOTIFIER_HEAD(h)	do { spin_lock_init(&(h)->lock); (h)->head = NULL; } while (0)
extern int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *nb);
extern int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *nb);
extern int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v);
extern int atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *nb);
extern int atomic_notifier_chain_unregister(struct atomic_notifier_head *nh, struct notifier_block *nb);
extern int atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v);
#define	notifier_from_errno(err)	((err) ? (NOTIFY_STOP_MASK | (NOTIFY_OK - (err))) : NOTIFY_OK)
#define	notifier_to_errno(ret)		((ret) & NOTIFY_STOP_MASK ? NOTIFY_OK - ((ret) & ~NOTIFY_STOP_MASK) : 0)
#endif
