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

#define	mutex_init(m)			lkpi_mutex_init(m)
#define	__mutex_init(m, n, k)		lkpi_mutex_init(m)
#define	mutex_init_with_key(m, k)	lkpi_mutex_init(m)
#define	mutex_destroy(m)		lkpi_mutex_destroy(m)
#define	mutex_lock(m)			mutex_enter(&(m)->m)
#define	mutex_lock_nested(m, s)		mutex_enter(&(m)->m)
#define	mutex_lock_nest_lock(m, n)	mutex_enter(&(m)->m)
#define	mutex_lock_io(m)		mutex_enter(&(m)->m)
#define	mutex_lock_interruptible(m)	({ mutex_enter(&(m)->m); 0; })
#define	mutex_lock_interruptible_nested(m, s)	mutex_lock_interruptible(m)
#define	mutex_lock_killable(m)		({ mutex_enter(&(m)->m); 0; })
#define	mutex_lock_killable_nested(m, s)	mutex_lock_killable(m)
#define	mutex_trylock(m)		(mutex_tryenter(&(m)->m) != 0)
#define	mutex_unlock(m)			mutex_exit(&(m)->m)
#define	mutex_is_locked(m)		lkpi_mutex_is_locked(m)
#define	mutex_lock_interruptible_io(m)	mutex_lock_interruptible(m)

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
