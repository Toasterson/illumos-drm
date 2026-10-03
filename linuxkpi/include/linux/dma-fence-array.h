/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's fence arrays (include/linux/dma-fence-array.h, GPL-2.0;
 * reimplemented in src/lkpi_fence_array.c as a driver-side fence over the
 * DRM facade): the array signals when all (or, with signal_on_any, one)
 * of its fences have signalled, with the first error. It takes over the
 * caller's references to the fences and the fences array.
 */
#ifndef _LINUX_DMA_FENCE_ARRAY_H
#define	_LINUX_DMA_FENCE_ARRAY_H

#include <linux/dma-fence.h>
#include <linux/irq_work.h>

struct dma_fence_array_cb {
	struct dma_fence_cb cb;
	struct dma_fence_array *array;
};

struct dma_fence_array {
	struct dma_fence base;
	spinlock_t lock;
	unsigned int num_fences;
	atomic_t num_pending;
	struct dma_fence **fences;
	struct irq_work work;
	struct dma_fence_array_cb callbacks[];
};

static inline struct dma_fence_array *
to_dma_fence_array(struct dma_fence *fence)
{
	if (fence == NULL || !dma_fence_is_array(fence))
		return (NULL);
	return (container_of(fence, struct dma_fence_array, base));
}

extern struct dma_fence_array *dma_fence_array_alloc(int num_fences);
extern void dma_fence_array_init(struct dma_fence_array *array,
    int num_fences, struct dma_fence **fences, u64 context, unsigned int seqno,
    bool signal_on_any);
extern struct dma_fence_array *dma_fence_array_create(int num_fences,
    struct dma_fence **fences, u64 context, unsigned int seqno,
    bool signal_on_any);
extern bool dma_fence_match_context(struct dma_fence *fence, u64 context);
extern struct dma_fence *dma_fence_array_first(struct dma_fence *head);
extern struct dma_fence *dma_fence_array_next(struct dma_fence *head,
    unsigned int index);

#define	dma_fence_array_for_each(fence, index, head) \
	for ((index) = 0, (fence) = dma_fence_array_first(head); (fence) != NULL; \
	    (fence) = dma_fence_array_next((head), ++(index)))

#endif
