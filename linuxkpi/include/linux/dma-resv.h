/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's reservation objects: the DRM facade's (<drm/dma_resv.h>,
 * misc/drm_core; dma_resv_init/fini/trylock are the facade's since 1.1)
 * plus the Linux helpers built on them in the layer (src/lkpi_resv.c):
 * iterators over one snapshot of the fences per walk
 * (drm_illumos_resv_fences(): each fence once, with its usage; locked
 * walks see signalled fences too), dma_resv_get_fences(),
 * dma_resv_get_singleton() (a fence array for several), wound/wait
 * transactions over the core's (drm_illumos_resv_lock_ctx()), and
 * dma_resv_held(). The fence lists themselves are the core's; a fence a
 * driver made comes back as that driver's own structure while it lives
 * (i915 recognises its requests), other fences as the core's structures.
 */
#ifndef _LINUX_DMA_RESV_H
#define	_LINUX_DMA_RESV_H

#include <linux/ww_mutex.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/seqlock.h>
#include <linux/rcupdate.h>
#include <drm/dma_resv.h>

extern struct ww_class reservation_ww_class;

#define	DMA_RESV_LIST_MASK	0x3

static inline enum dma_resv_usage
dma_resv_usage_rw(bool write)
{
	return (write ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE);
}

struct dma_resv_iter {
	struct dma_resv *obj;
	enum dma_resv_usage usage;
	struct dma_fence *fence;
	enum dma_resv_usage fence_usage;
	unsigned int index;
	struct dma_fence **fences;
	unsigned int num_fences;
	bool is_restarted;
	bool locked;
	u8 *lkpi_usages;
	/*
	 * illumos: the snapshot the walk runs on. A locked walk
	 * (dma_resv_for_each_fence()) releases it at its end or when the
	 * object is unlocked, so it needs no dma_resv_iter_end(), as on Linux;
	 * do not call dma_resv_iter_end() on a locked walk after the unlock.
	 * An unlocked walk ends with dma_resv_iter_end().
	 */
	void *lkpi_snap;
};

extern struct dma_fence *dma_resv_iter_first_unlocked(
    struct dma_resv_iter *cursor);
extern struct dma_fence *dma_resv_iter_next_unlocked(
    struct dma_resv_iter *cursor);
extern struct dma_fence *dma_resv_iter_first(struct dma_resv_iter *cursor);
extern struct dma_fence *dma_resv_iter_next(struct dma_resv_iter *cursor);
extern void dma_resv_iter_end(struct dma_resv_iter *cursor);
extern int dma_resv_get_fences(struct dma_resv *obj,
    enum dma_resv_usage usage, unsigned int *num_fences,
    struct dma_fence ***fences);
extern int dma_resv_get_singleton(struct dma_resv *obj,
    enum dma_resv_usage usage, struct dma_fence **fence);
extern int lkpi_dma_resv_lock(struct dma_resv *obj,
    struct ww_acquire_ctx *ctx, bool intr);
extern int lkpi_dma_resv_lock_slow(struct dma_resv *obj,
    struct ww_acquire_ctx *ctx, bool intr);
extern void lkpi_dma_resv_unlock(struct dma_resv *obj);
extern bool lkpi_dma_resv_held(struct dma_resv *obj);
extern void dma_resv_describe(struct dma_resv *obj, struct seq_file *seq);
extern void dma_resv_set_deadline(struct dma_resv *obj,
    enum dma_resv_usage usage, ktime_t deadline);
extern void dma_resv_replace_fences(struct dma_resv *obj, uint64_t context,
    struct dma_fence *fence, enum dma_resv_usage usage);
extern int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src);

static inline void
dma_resv_iter_begin(struct dma_resv_iter *cursor, struct dma_resv *obj,
    enum dma_resv_usage usage)
{
	memset(cursor, 0, sizeof (*cursor));
	cursor->obj = obj;
	cursor->usage = usage;
}

static inline enum dma_resv_usage
dma_resv_iter_usage(struct dma_resv_iter *cursor)
{
	return (cursor->fence_usage);
}

static inline bool
dma_resv_iter_is_restarted(struct dma_resv_iter *cursor)
{
	return (cursor->is_restarted);
}

#define	dma_resv_for_each_fence_unlocked(cursor, fence) \
	for ((fence) = dma_resv_iter_first_unlocked(cursor); (fence) != NULL; \
	    (fence) = dma_resv_iter_next_unlocked(cursor))
#define	dma_resv_for_each_fence(cursor, obj, usage, fence) \
	for (dma_resv_iter_begin(cursor, obj, usage), \
	    (fence) = dma_resv_iter_first(cursor); (fence) != NULL; \
	    (fence) = dma_resv_iter_next(cursor))

#define	dma_resv_lock(obj, ctx)		lkpi_dma_resv_lock((obj), (ctx), false)
#define	dma_resv_lock_interruptible(obj, ctx)	lkpi_dma_resv_lock((obj), (ctx), true)
#define	dma_resv_lock_slow(obj, ctx)	((void)lkpi_dma_resv_lock_slow((obj), (ctx), false))
#define	dma_resv_lock_slow_interruptible(obj, ctx)	lkpi_dma_resv_lock_slow((obj), (ctx), true)
#define	dma_resv_unlock(obj)		lkpi_dma_resv_unlock(obj)
#define	dma_resv_held(obj)		lkpi_dma_resv_held(obj)
#define	dma_resv_assert_held(obj)	((void)(obj))
#define	dma_resv_locking_ctx(obj)	((struct ww_acquire_ctx *)(obj)->lock.ctx)
#define	dma_resv_reset_max_fences(obj)	((void)(obj))

#endif
