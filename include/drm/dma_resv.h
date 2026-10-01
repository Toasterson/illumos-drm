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

#ifdef	__cplusplus
}
#endif

#endif	/* _DMA_RESV_H */
