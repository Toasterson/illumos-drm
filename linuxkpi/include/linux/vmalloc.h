/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's vmalloc() and vmap() (include/linux/vmalloc.h, GPL-2.0;
 * reimplemented). vmalloc() is kmalloc() (illumos kmem_alloc(9F) is
 * already virtually contiguous for any size). vmap() maps pages at a
 * contiguous kernel address (src/lkpi_page.c): pages that already are
 * virtually contiguous are returned as they are; otherwise a range of
 * kernel heap is loaded with the pages' frames.
 */
#ifndef _LINUX_VMALLOC_H
#define	_LINUX_VMALLOC_H

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm_types.h>
#include <asm/pgtable_types.h>

#define	VM_MAP		0x00000004
#define	VM_IOREMAP	0x00000001
#define	VM_ALLOC	0x00000002
#define	VM_USERMAP	0x00000008
#define	VM_DMA_COHERENT	0x00000010
#define	VM_MAP_PUT_PAGES	0x00000200	/* vunmap() puts the pages, frees the array */

#define	vmalloc(size)			lkpi_kmalloc((size), GFP_KERNEL)
#define	vzalloc(size)			lkpi_kmalloc((size), GFP_KERNEL | __GFP_ZERO)
#define	vmalloc_user(size)		vzalloc(size)
#define	vmalloc_32(size)		vmalloc(size)
#define	__vmalloc(size, gfp)		lkpi_kmalloc((size), (gfp))
#define	vmalloc_node(size, n)		vmalloc(size)
#define	vzalloc_node(size, n)		vzalloc(size)
#define	vmalloc_array(n, size)		vmalloc(size_mul((n), (size)))
#define	vcalloc(n, size)		vzalloc(size_mul((n), (size)))
#define	vfree(p)			lkpi_kfree(p)
/*
 * Linux: an address inside the vmalloc area. Here: inside a live vmap() or
 * vmap_pfn() mapping (including the contiguous ones that are the pages' own
 * address), so that a caller that unmaps only what it mapped does so: i915's
 * unmap_object() calls vunmap() only when this is true (with "false", every
 * pinned mapping of a GEM object outlived its freed pages). vmalloc() memory
 * is kmem here and answers false; vfree() and kvfree() free either kind.
 */
extern bool lkpi_is_vmap_addr(const void *addr);
#define	is_vmalloc_addr(p)		lkpi_is_vmap_addr(p)

/* No vmap-space pressure notifications on illumos: nothing to register. */
#define	register_vmap_purge_notifier(nb)	((void)(nb), 0)
#define	unregister_vmap_purge_notifier(nb)	((void)(nb), 0)

extern void *vmap(struct lx_page **pages, unsigned int count,
    unsigned long flags, pgprot_t prot);
extern void vunmap(const void *addr);
extern void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot);
extern struct lx_page *vmalloc_to_page(const void *addr);

#endif
