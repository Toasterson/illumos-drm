/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's refcount_t (include/linux/refcount.h, GPL-2.0; reimplemented
 * on the facade's refcount_t). As on Linux the count saturates instead of
 * overflowing, and a decrement from zero or an increment from zero warns
 * and leaves the count saturated (REFCOUNT_SATURATED), so the object
 * leaks instead of being freed twice.
 */
#ifndef _LINUX_REFCOUNT_H
#define	_LINUX_REFCOUNT_H

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/limits.h>

#define	REFCOUNT_INIT(n)	{ .refs = ATOMIC_INIT(n) }
#define	REFCOUNT_MAX		INT_MAX
#define	REFCOUNT_SATURATED	(INT_MIN / 2)

enum refcount_saturation_type {
	REFCOUNT_ADD_NOT_ZERO_OVF,
	REFCOUNT_ADD_OVF,
	REFCOUNT_ADD_UAF,
	REFCOUNT_SUB_UAF,
	REFCOUNT_DEC_LEAK
};

extern void refcount_warn_saturate(refcount_t *r,
    enum refcount_saturation_type t);

static inline void
refcount_set(refcount_t *r, int n)
{
	atomic_set(&r->refs, n);
}

static inline void
refcount_set_release(refcount_t *r, int n)
{
	atomic_set_release(&r->refs, n);
}

static inline unsigned int
refcount_read(const refcount_t *r)
{
	return ((unsigned int)atomic_read(&r->refs));
}

static inline bool
__refcount_add_not_zero(int i, refcount_t *r, int *oldp)
{
	int old = refcount_read(r);

	do {
		if (old == 0)
			break;
	} while (!atomic_try_cmpxchg(&r->refs, &old, old + i));
	if (oldp != NULL)
		*oldp = old;
	if (unlikely(old < 0 || old + i < 0))
		refcount_warn_saturate(r, REFCOUNT_ADD_NOT_ZERO_OVF);
	return (old != 0);
}

static inline bool
refcount_add_not_zero(int i, refcount_t *r)
{
	return (__refcount_add_not_zero(i, r, NULL));
}

static inline bool
refcount_inc_not_zero(refcount_t *r)
{
	return (__refcount_add_not_zero(1, r, NULL));
}

static inline bool
__refcount_inc_not_zero(refcount_t *r, int *oldp)
{
	return (__refcount_add_not_zero(1, r, oldp));
}

static inline void
__refcount_add(int i, refcount_t *r, int *oldp)
{
	int old = atomic_fetch_add(i, &r->refs);

	if (oldp != NULL)
		*oldp = old;
	if (unlikely(old == 0))
		refcount_warn_saturate(r, REFCOUNT_ADD_UAF);
	else if (unlikely(old < 0 || old + i < 0))
		refcount_warn_saturate(r, REFCOUNT_ADD_OVF);
}

static inline void
refcount_add(int i, refcount_t *r)
{
	__refcount_add(i, r, NULL);
}

static inline void
refcount_inc(refcount_t *r)
{
	__refcount_add(1, r, NULL);
}

static inline void
__refcount_inc(refcount_t *r, int *oldp)
{
	__refcount_add(1, r, oldp);
}

static inline bool
__refcount_sub_and_test(int i, refcount_t *r, int *oldp)
{
	int old = atomic_fetch_sub(i, &r->refs);

	if (oldp != NULL)
		*oldp = old;
	if (old == i) {
		smp_acquire__after_ctrl_dep();
		return (true);
	}
	if (unlikely(old < 0 || old - i < 0))
		refcount_warn_saturate(r, REFCOUNT_SUB_UAF);
	return (false);
}

static inline bool
refcount_sub_and_test(int i, refcount_t *r)
{
	return (__refcount_sub_and_test(i, r, NULL));
}

static inline bool
refcount_dec_and_test(refcount_t *r)
{
	return (__refcount_sub_and_test(1, r, NULL));
}

static inline void
refcount_dec(refcount_t *r)
{
	if (unlikely(atomic_fetch_sub(1, &r->refs) <= 1))
		refcount_warn_saturate(r, REFCOUNT_DEC_LEAK);
}

static inline bool
refcount_dec_if_one(refcount_t *r)
{
	int val = 1;

	return (atomic_try_cmpxchg(&r->refs, &val, 0));
}

static inline bool
refcount_dec_not_one(refcount_t *r)
{
	int val = atomic_read(&r->refs);

	do {
		if (unlikely(val == REFCOUNT_SATURATED))
			return (true);
		if (val == 1)
			return (false);
		if (unlikely(val == 0)) {
			refcount_warn_saturate(r, REFCOUNT_SUB_UAF);
			return (true);
		}
	} while (!atomic_try_cmpxchg(&r->refs, &val, val - 1));
	return (true);
}

struct lx_mutex;
struct spinlock;
extern bool refcount_dec_and_mutex_lock(refcount_t *r, struct lx_mutex *lock);
extern bool refcount_dec_and_lock(refcount_t *r, struct spinlock *lock);
extern bool refcount_dec_and_lock_irqsave(refcount_t *r,
    struct spinlock *lock, unsigned long *flags);

#endif
