/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's page allocator interface (include/linux/gfp.h, GPL-2.0;
 * reimplemented in src/lkpi_page.c). See <linux/mm_types.h> for the page
 * model. alloc_pages() of order 0 returns a page-aligned page from a
 * kmem cache; higher orders return NULL ("not supported"), because illumos
 * kernel memory is not physically contiguous beyond a page and the layer
 * gives each struct page one DMA address. Drivers that fall back to
 * smaller orders (i915 does) then use single pages; physically
 * contiguous DMA memory comes from dma_alloc_coherent().
 */
#ifndef _LINUX_GFP_H
#define	_LINUX_GFP_H

#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/mmzone.h>

struct lx_page;

static inline int
lkpi_kmflag(gfp_t gfp)
{
	return ((gfp & __GFP_DIRECT_RECLAIM) ? KM_SLEEP : KM_NOSLEEP);
}

static inline bool
gfpflags_allow_blocking(const gfp_t gfp)
{
	return ((gfp & __GFP_DIRECT_RECLAIM) != 0);
}

extern struct lx_page *alloc_pages(gfp_t gfp, unsigned int order);
extern void __free_pages(struct lx_page *page, unsigned int order);
extern unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
extern unsigned long get_zeroed_page(gfp_t gfp);
extern void free_pages(unsigned long addr, unsigned int order);
extern void *alloc_pages_exact(size_t size, gfp_t gfp);
extern void free_pages_exact(void *virt, size_t size);

#define	alloc_page(gfp)			alloc_pages((gfp), 0)
#define	alloc_pages_node(nid, gfp, order)	alloc_pages((gfp), (order))
#define	__alloc_pages(gfp, order, nid, nm)	alloc_pages((gfp), (order))
#define	__get_free_page(gfp)		__get_free_pages((gfp), 0)
#define	__free_page(page)		__free_pages((page), 0)
#define	free_page(addr)			free_pages((addr), 0)
#define	gfp_zone(gfp)			0
#define	numa_node_id()			0
#define	NUMA_NO_NODE			(-1)
#ifndef dev_to_node	/* <linux/device.h> defines it too */
#define	dev_to_node(dev)		NUMA_NO_NODE
#endif

#endif
