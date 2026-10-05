/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Reservation objects in the illumos-drm facade.
 *
 * Origin: struct dma_resv, enum dma_resv_usage and the functions below
 * mirror the names, field order and semantics of Linux v7.2's
 * include/linux/dma-resv.h. That header is GPL-2.0 (with an MIT-style
 * dual notice in parts); this file is a reimplementation for illumos
 * from the documented interface, not a copy, and is MIT-licensed
 * (decision D7 of illumos-drm, 2026-10-01).
 */

#ifndef _DMA_RESV_H
#define	_DMA_RESV_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * How a fence uses a buffer; Linux's order, from strongest to weakest.
 * The functions refuse any other value (with a warning): add_fence waits
 * for the fence instead, wait_timeout returns -EINVAL and test_signaled
 * returns false.
 */
enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL = 0,
	DMA_RESV_USAGE_WRITE = 1,
	DMA_RESV_USAGE_READ = 2,
	DMA_RESV_USAGE_BOOKKEEP = 3
};

/*
 * A reservation object: the fences of a buffer and the lock that
 * serialises changing them. Every GEM object has one (_resv, reached
 * through obj->resv). In facade version 0 the fences and the
 * wound/wait lock live in the core (__core); `lock` and `fences` are
 * placeholders.
 */
struct dma_resv {
	struct ww_mutex lock;
	void *fences;
	/* private: the core's state */
	uint64_t __core[2];
};

/*
 * Locks the object. Facade version 0 takes only ctx == NULL (one object
 * at a time); a non-NULL acquire context fails with -EINVAL. Returns 0,
 * -EINTR (dma_resv_lock_interruptible()) or -EDEADLK if the calling
 * thread holds the lock already (where Linux would deadlock). The lock
 * belongs to the calling thread: only it may add fences and unlock, and
 * dma_resv_unlock() by another thread is ignored with a warning. Thread
 * context only.
 */
extern int dma_resv_lock(struct dma_resv *obj, struct ww_acquire_ctx *ctx);
extern int dma_resv_lock_interruptible(struct dma_resv *obj,
    struct ww_acquire_ctx *ctx);
extern void dma_resv_unlock(struct dma_resv *obj);

/*
 * Makes room for `num_fences` more fences; locked. Returns 0 or -ENOMEM.
 */
extern int dma_resv_reserve_fences(struct dma_resv *obj,
    unsigned int num_fences);

/*
 * Adds a fence with a usage; locked by the calling thread, after
 * dma_resv_reserve_fences(). A
 * fence of a context the object already has replaces the older one. If
 * the room was not reserved, or the lock is not held by the calling
 * thread, the fence is waited for instead (with a warning), which keeps
 * the ordering.
 */
extern void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
    enum dma_resv_usage usage);

/*
 * Waits for the object's fences of `usage` and stronger. Returns the
 * remaining clock ticks (at least 1), 0 on timeout, -ERESTARTSYS
 * (-EINTR). Thread context only; not locked.
 */
extern long dma_resv_wait_timeout(struct dma_resv *obj,
    enum dma_resv_usage usage, bool intr, unsigned long timeout);

/* Whether every fence of `usage` and stronger has signalled. */
extern bool dma_resv_test_signaled(struct dma_resv *obj,
    enum dma_resv_usage usage);

/*
 * Facade 1.1.
 *
 * dma_resv_init(), dma_resv_fini()
 *	a driver's own reservation (not a GEM object's _resv): init gives it
 *	a core reservation, fini releases it (a GEM object's reservation is
 *	the core's: fini does nothing for it). Thread context; init waits
 *	for memory.
 * dma_resv_trylock()
 *	takes the lock without waiting, as dma_resv_lock(obj, NULL) would;
 *	false if anyone holds it.
 * drm_illumos_ww_stamp(), drm_illumos_resv_lock_ctx(),
 * drm_illumos_resv_unlock_ctx()
 *	transactions that a compatibility layer keeps (Linux's
 *	ww_acquire_ctx): a stamp from the core's reservation class, then
 *	lock with the stamp and the number of reservations the transaction
 *	holds; the core applies its wait-die rule against every holder.
 *	0, -EDEADLK (release everything, then lock the contended one with
 *	slow), -EALREADY (held by the transaction), -EINTR (intr), -EINVAL.
 *	The lock belongs to the calling thread; release it with
 *	drm_illumos_resv_unlock_ctx() and the same stamp.
 *	drm_illumos_resv_trylock_ctx() takes it for the stamp without
 *	waiting (false if held); drm_illumos_resv_is_locked() reports
 *	whether anyone holds it (assertions).
 * drm_illumos_resv_fences()
 *	a snapshot of the unsignalled fences with a usage at least as
 *	important as `usage`: up to `max` of them, each with a new reference,
 *	into `fences`, and the number the reservation had into `*count`
 *	(retry with a larger array when it exceeds max; the stored fences are
 *	the caller's to put either way). The structures are the core's views
 *	of the fences, not a driver's own structures. Thread context.
 */
extern void dma_resv_init(struct dma_resv *obj);
extern void dma_resv_fini(struct dma_resv *obj);
extern bool dma_resv_trylock(struct dma_resv *obj);
extern uint64_t drm_illumos_ww_stamp(void);
extern int drm_illumos_resv_lock_ctx(struct dma_resv *obj, uint64_t stamp,
    uint32_t acquired, bool slow, bool intr);
extern bool drm_illumos_resv_trylock_ctx(struct dma_resv *obj, uint64_t stamp);
extern void drm_illumos_resv_unlock_ctx(struct dma_resv *obj, uint64_t stamp);
extern bool drm_illumos_resv_is_locked(struct dma_resv *obj);
extern int drm_illumos_resv_fences(struct dma_resv *obj,
    enum dma_resv_usage usage, struct dma_fence **fences, uint32_t max,
    uint32_t *count);

#ifdef	__cplusplus
}
#endif

#endif	/* _DMA_RESV_H */
