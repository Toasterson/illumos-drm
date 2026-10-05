/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct mutex over an illumos kmutex_t (include/linux/mutex.h,
 * GPL-2.0; reimplemented). struct mutex is the DRM facade's struct
 * lx_mutex (the prologue renames the tag; illumos' kmutex_t is itself
 * "struct mutex").
 *
 * illumos kmutexes cannot be interrupted by signals: the _interruptible
 * and _killable forms wait like mutex_lock() and return 0, which Linux
 * callers handle (success is always a possible outcome). A zero-filled
 * mutex is valid (DEFINE_MUTEX() is an empty initialiser). Locking a
 * mutex the thread already holds panics, as illumos does for any kmutex.
 * mutex_is_locked() is a snapshot for assertions.
 */
#ifndef _LINUX_MUTEX_H
#define	_LINUX_MUTEX_H

#include <linux/types.h>
#include <linux/lockdep.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>
#include <linux/spinlock_types.h>

#define	DEFINE_MUTEX(name)	struct mutex name = { }
#define	__MUTEX_INITIALIZER(name)	{ }

static inline void
lkpi_mutex_init(struct mutex *m)
{
	(mutex_init)(&m->m, NULL, MUTEX_DRIVER, NULL);
}

static inline void
lkpi_mutex_destroy(struct mutex *m)
{
	(mutex_destroy)(&m->m);
}

static inline bool
lkpi_mutex_is_locked(struct mutex *m)
{
	if (mutex_owned(&m->m))
		return (true);
	if (mutex_tryenter(&m->m) != 0) {
		mutex_exit(&m->m);
		return (false);
	}
	return (true);
}

#define	mutex_init(lk)			lkpi_mutex_init(lk)
#define	__mutex_init(lk, n, k)		lkpi_mutex_init(lk)
#define	mutex_init_with_key(lk, k)	lkpi_mutex_init(lk)
#define	mutex_destroy(lk)		lkpi_mutex_destroy(lk)
#define	mutex_lock(lk)			mutex_enter(&(lk)->m)
#define	mutex_lock_nested(lk, s)		mutex_enter(&(lk)->m)
#define	mutex_lock_nest_lock(lk, n)	mutex_enter(&(lk)->m)
#define	mutex_lock_io(lk)		mutex_enter(&(lk)->m)
#define	mutex_lock_interruptible(lk)	({ mutex_enter(&(lk)->m); 0; })
#define	mutex_lock_interruptible_nested(lk, s)	mutex_lock_interruptible(lk)
#define	mutex_lock_killable(lk)		({ mutex_enter(&(lk)->m); 0; })
#define	mutex_lock_killable_nested(lk, s)	mutex_lock_killable(lk)
#define	mutex_trylock(lk)		(mutex_tryenter(&(lk)->m) != 0)
#define	mutex_unlock(lk)			mutex_exit(&(lk)->m)
#define	mutex_is_locked(lk)		lkpi_mutex_is_locked(lk)
#define	mutex_lock_interruptible_io(lk)	mutex_lock_interruptible(lk)

static inline int
lkpi_atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	if (atomic_add_unless(cnt, -1, 1))
		return (0);
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		mutex_unlock(lock);
		return (0);
	}
	return (1);
}
#define	atomic_dec_and_mutex_lock(c, l)	lkpi_atomic_dec_and_mutex_lock(c, l)

DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))
DEFINE_GUARD_COND(mutex, _try, mutex_trylock(_T))
DEFINE_GUARD_COND(mutex, _intr, mutex_lock_interruptible(_T) == 0)

#endif
