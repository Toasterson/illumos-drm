/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's spinlocks over illumos adaptive kmutexes (include/linux/
 * spinlock.h, GPL-2.0; reimplemented).
 *
 * Every Linux spinlock, raw or not, is a kmutex_t initialised with
 * MUTEX_DRIVER and no interrupt priority, which makes it an adaptive
 * mutex. That is valid in interrupt handlers because the layer only
 * registers handlers below LOCK_LEVEL (request_irq() refuses high-level
 * interrupts), where illumos runs handlers as interrupt threads that may
 * block on an adaptive mutex. The _irq/_irqsave/_bh forms therefore only
 * take the mutex: illumos needs no interrupt masking to prevent a handler
 * from deadlocking against the thread it interrupted. `flags` is set to 0.
 * As on Linux, the holder must not sleep and must not take the lock
 * again; a spinlock may be zero-filled instead of initialised.
 */
#ifndef _LINUX_SPINLOCK_H
#define	_LINUX_SPINLOCK_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/spinlock_types.h>
#include <linux/lockdep.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/bottom_half.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>

static inline void
lkpi_spin_lock_init(spinlock_t *lock)
{
	(mutex_init)(&lock->rlock, NULL, MUTEX_DRIVER, NULL);
}

#define	spin_lock_init(l)		lkpi_spin_lock_init(l)
#define	raw_spin_lock_init(l)		(mutex_init)(&(l)->rlock, NULL, MUTEX_DRIVER, NULL)
#define	spin_lock(l)			mutex_enter(&(l)->rlock)
#define	spin_unlock(l)			mutex_exit(&(l)->rlock)
#define	spin_lock_nested(l, s)		mutex_enter(&(l)->rlock)
#define	spin_lock_nest_lock(l, n)	mutex_enter(&(l)->rlock)
#define	spin_lock_irq(l)		mutex_enter(&(l)->rlock)
#define	spin_unlock_irq(l)		mutex_exit(&(l)->rlock)
#define	spin_lock_bh(l)			mutex_enter(&(l)->rlock)
#define	spin_unlock_bh(l)		mutex_exit(&(l)->rlock)
#define	spin_lock_irqsave(l, flags) \
	do { (flags) = 0; mutex_enter(&(l)->rlock); } while (0)
#define	spin_lock_irqsave_nested(l, flags, s) spin_lock_irqsave(l, flags)
#define	spin_unlock_irqrestore(l, flags) \
	do { (void)(flags); mutex_exit(&(l)->rlock); } while (0)
#define	spin_trylock(l)			(mutex_tryenter(&(l)->rlock) != 0)
#define	spin_trylock_irq(l)		spin_trylock(l)
#define	spin_trylock_bh(l)		spin_trylock(l)
#define	spin_trylock_irqsave(l, flags) \
	({ (flags) = 0; spin_trylock(l); })
#define	spin_is_locked(l)		lkpi_spin_is_locked(l)
#define	assert_spin_locked(l)		ASSERT(MUTEX_HELD(&(l)->rlock))

#define	raw_spin_lock(l)		spin_lock(l)
#define	raw_spin_unlock(l)		spin_unlock(l)
#define	raw_spin_lock_irq(l)		spin_lock_irq(l)
#define	raw_spin_unlock_irq(l)		spin_unlock_irq(l)
#define	raw_spin_lock_irqsave(l, f)	spin_lock_irqsave(l, f)
#define	raw_spin_unlock_irqrestore(l, f) spin_unlock_irqrestore(l, f)
#define	raw_spin_trylock(l)		spin_trylock(l)

/* Whether anyone holds the lock (a racy snapshot, for assertions). */
static inline bool
lkpi_spin_is_locked(spinlock_t *l)
{
	if (MUTEX_HELD(&l->rlock))
		return (true);
	if (mutex_tryenter(&l->rlock) != 0) {
		mutex_exit(&l->rlock);
		return (false);
	}
	return (true);
}

/* --- rwlock_t: an illumos krwlock_t ----------------------------------- */

#define	rwlock_init(l)		rw_init(&(l)->rw, NULL, RW_DRIVER, NULL)
#define	read_lock(l)		rw_enter(&(l)->rw, RW_READER)
#define	read_unlock(l)		rw_exit(&(l)->rw)
#define	write_lock(l)		rw_enter(&(l)->rw, RW_WRITER)
#define	write_unlock(l)		rw_exit(&(l)->rw)
#define	read_lock_irq(l)	read_lock(l)
#define	read_unlock_irq(l)	read_unlock(l)
#define	write_lock_irq(l)	write_lock(l)
#define	write_unlock_irq(l)	write_unlock(l)
#define	read_lock_irqsave(l, f)	do { (f) = 0; read_lock(l); } while (0)
#define	read_unlock_irqrestore(l, f)	do { (void)(f); read_unlock(l); } while (0)
#define	write_lock_irqsave(l, f)	do { (f) = 0; write_lock(l); } while (0)
#define	write_unlock_irqrestore(l, f)	do { (void)(f); write_unlock(l); } while (0)

/* atomic_dec_and_lock(): decrement, and take the lock if it reached 0. */
static inline int
lkpi_atomic_dec_and_lock(atomic_t *a, spinlock_t *l)
{
	if (atomic_add_unless(a, -1, 1))
		return (0);
	spin_lock(l);
	if (atomic_dec_and_test(a))
		return (1);
	spin_unlock(l);
	return (0);
}
#define	atomic_dec_and_lock(a, l)	lkpi_atomic_dec_and_lock(a, l)
#define	atomic_dec_and_lock_irqsave(a, l, f) \
	({ (f) = 0; lkpi_atomic_dec_and_lock(a, l); })

DEFINE_LOCK_GUARD_1(spinlock, spinlock_t, spin_lock(_T->lock), spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t, spin_lock_irq(_T->lock), spin_unlock_irq(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t,
    spin_lock_irqsave(_T->lock, _T->flags), spin_unlock_irqrestore(_T->lock, _T->flags),
    unsigned long flags)
DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t, raw_spin_lock(_T->lock), raw_spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t,
    raw_spin_lock_irqsave(_T->lock, _T->flags), raw_spin_unlock_irqrestore(_T->lock, _T->flags),
    unsigned long flags)
DEFINE_LOCK_GUARD_1(read_lock, rwlock_t, read_lock(_T->lock), read_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(write_lock, rwlock_t, write_lock(_T->lock), write_unlock(_T->lock))

#endif
