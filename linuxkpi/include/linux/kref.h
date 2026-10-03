/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct kref (include/linux/kref.h, GPL-2.0; reimplemented).
 * kref_read(), kref_get() and kref_get_unless_zero() are the DRM
 * facade's (<drm/drm_facade_types.h>), which are the same operations on
 * the same count; this adds kref_init() and the put family for the
 * driver's own objects. Facade objects (GEM objects, fences) are released
 * with their own put functions, never with kref_put().
 */
#ifndef _LINUX_KREF_H
#define	_LINUX_KREF_H

#include <linux/refcount.h>
#include <linux/spinlock_types.h>

#define	KREF_INIT(n)	{ .refcount = REFCOUNT_INIT(n) }

static inline void
kref_init(struct kref *kref)
{
	refcount_set(&kref->refcount, 1);
}

static inline int
kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
	if (refcount_dec_and_test(&kref->refcount)) {
		release(kref);
		return (1);
	}
	return (0);
}

static inline int
kref_put_mutex(struct kref *kref, void (*release)(struct kref *kref),
    struct lx_mutex *lock)
{
	if (refcount_dec_and_mutex_lock(&kref->refcount, lock)) {
		release(kref);
		return (1);
	}
	return (0);
}

static inline int
kref_put_lock(struct kref *kref, void (*release)(struct kref *kref),
    spinlock_t *lock)
{
	if (refcount_dec_and_lock(&kref->refcount, lock)) {
		release(kref);
		return (1);
	}
	return (0);
}

#endif
