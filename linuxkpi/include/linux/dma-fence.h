/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <linux/dma-fence.h>: the DRM facade's fences (<drm/dma_fence.h>,
 * implemented by misc/drm_core) plus the Linux helpers the facade leaves
 * to the compatibility layer. RCU lookups of fences
 * (dma_fence_get_rcu_safe()) work because the core releases a driver
 * fence's memory only through the driver's release op, which i915 defers
 * with an RCU callback (the layer's call_rcu()).
 */
#ifndef _LINUX_DMA_FENCE_H
#define	_LINUX_DMA_FENCE_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/printk.h>
#include <drm/dma_fence.h>

struct seq_file;

static inline struct dma_fence *
dma_fence_get_rcu(struct dma_fence *fence)
{
	if (kref_get_unless_zero(&fence->refcount))
		return (fence);
	return (NULL);
}

static inline struct dma_fence *
dma_fence_get_rcu_safe(struct dma_fence __rcu **fencep)
{
	for (;;) {
		struct dma_fence *fence = rcu_dereference(*fencep);

		if (fence == NULL || dma_fence_get_rcu(fence) == NULL) {
			if (fence == NULL)
				return (NULL);
			continue;
		}
		if (fence == rcu_access_pointer(*fencep))
			return (fence);
		dma_fence_put(fence);
	}
}

static inline bool
dma_fence_is_signaled_flag_lkpi(struct dma_fence *fence)
{
	return (dma_fence_test_signaled_flag(fence));
}

extern void dma_fence_free(struct dma_fence *fence);
extern void dma_fence_release(struct kref *kref);
extern void dma_fence_describe(struct dma_fence *fence, struct seq_file *seq);
extern signed long dma_fence_wait_any_timeout(struct dma_fence **fences,
    uint32_t count, bool intr, signed long timeout, uint32_t *idx);
extern struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp);
extern const struct dma_fence_ops dma_fence_array_ops;
extern const struct dma_fence_ops dma_fence_chain_ops;

#define	dma_fence_is_array(f)		((f)->ops == &dma_fence_array_ops)
#define	dma_fence_is_chain(f)		((f)->ops == &dma_fence_chain_ops)
#define	dma_fence_is_container(f)	(dma_fence_is_array(f) || dma_fence_is_chain(f))
#define	DMA_FENCE_WARN(f, fmt, args...) \
	pr_warn("f %llu#%llu: " fmt, (unsigned long long)(f)->context, \
	    (unsigned long long)(f)->seqno, ##args)

#endif
