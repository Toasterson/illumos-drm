/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's fence chains (timeline points). Chains belong to the DRM core
 * (the facade's dma_fence_chain_alloc()/drm_syncobj_add_point()); their
 * memory is the core's and C code must not read it. The structure is
 * Linux's so that drivers compile; to_dma_fence_chain() is NULL for every
 * fence the layer sees, so dma_fence_chain_find_seqno() behaves as Linux
 * does for a fence that is not a chain (a point other than 0 is -EINVAL),
 * and dma_fence_chain_walk() ends the walk. Timeline waits therefore go
 * through drm_syncobj_find_fence() with the point, which the core
 * resolves.
 */
#ifndef _LINUX_DMA_FENCE_CHAIN_H
#define	_LINUX_DMA_FENCE_CHAIN_H

#include <linux/dma-fence.h>
#include <linux/irq_work.h>
#include <linux/slab.h>

struct dma_fence_chain {
	struct dma_fence base;
	struct dma_fence __rcu *prev;
	u64 prev_seqno;
	struct dma_fence *fence;
	union {
		struct dma_fence_cb cb;
		struct irq_work work;
	};
	spinlock_t lock;
};

static inline struct dma_fence_chain *
to_dma_fence_chain(struct dma_fence *fence)
{
	if (fence == NULL || !dma_fence_is_chain(fence))
		return (NULL);
	return (container_of(fence, struct dma_fence_chain, base));
}

extern struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence);
extern int dma_fence_chain_find_seqno(struct dma_fence **pfence,
    uint64_t seqno);
extern void dma_fence_chain_init(struct dma_fence_chain *chain,
    struct dma_fence *prev, struct dma_fence *fence, uint64_t seqno);

#define	dma_fence_chain_contained(f) \
	(to_dma_fence_chain(f) ? to_dma_fence_chain(f)->fence : (f))
#define	dma_fence_chain_for_each(iter, head) \
	for ((iter) = dma_fence_get(head); (iter) != NULL; \
	    (iter) = dma_fence_chain_walk(iter))

#endif
