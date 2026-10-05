/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Interfaces between the memory parts of the LinuxKPI layer (src/lkpi_page.c,
 * lkpi_slab.c, lkpi_shmem.c, lkpi_mm.c, lkpi_scatterlist.c, lkpi_dma.c,
 * lkpi_io.c); not for drivers.
 *
 * Every layer page (<linux/mm_types.h>) is a descriptor allocated by
 * lkpi_page_create() for a page-aligned kernel virtual address, with an
 * owner that decides what happens when its last reference is dropped
 * (put_page() -> lkpi_page_release() -> owner->lpo_release):
 *
 *	alloc_pages()	the page memory is freed with the descriptor;
 *	shmem files	the descriptor holds a reference on the file's backing
 *			allocation, which is freed with its last page;
 *	pinned user	the descriptor holds a reference on the pin, which is
 *			unmapped and unlocked with its last page;
 *	foreign		kernel memory the layer did not allocate (kmalloc()
 *			memory given to virt_to_page()), until that memory
 *			is kfree()d: see lkpi_page.c.
 *
 * All descriptors with a registered address are kept in one index
 * (address -> page), which is how virt_to_page() finds "the" struct page
 * of a kernel address.
 */
#ifndef _LKPI_MEM_H
#define	_LKPI_MEM_H

#include <linux/mm.h>
#include <linux/scatterlist.h>

/*
 * Page arithmetic with the layer's constant PAGE_SHIFT: illumos' PAGESIZE,
 * ptob() and btop() are variables and functions outside the DDI here.
 */
#define	LKPI_PTOB(n)	((size_t)(n) << PAGE_SHIFT)
#define	LKPI_BTOP(b)	((size_t)(b) >> PAGE_SHIFT)
#define	LKPI_BTOPR(b)	(((size_t)(b) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define	LKPI_PAGEOFF(a)	((uintptr_t)(a) & (PAGE_SIZE - 1))

struct lkpi_page_owner {
	/* Called once the page's reference count reached zero. */
	void (*lpo_release)(struct lkpi_page_owner *owner,
	    struct lx_page *page);
	const char *lpo_name;
};

/*
 * A descriptor for the page at the page-aligned `kva`, with one reference
 * and registered in the address index (replacing a foreign descriptor of
 * that address). NULL only if kmflag is KM_NOSLEEP and memory is short.
 */
extern struct lx_page *lkpi_page_create(void *kva,
    struct lkpi_page_owner *owner, int kmflag);
/* Removes the descriptor from the index and frees it (not the memory). */
extern void lkpi_page_destroy(struct lx_page *page);
/* The registered descriptor of a page-aligned address, or NULL. */
extern struct lx_page *lkpi_page_lookup(const void *kva);
/*
 * Memory [addr, addr + len) is being freed: drops the index's reference
 * on the foreign descriptors of the pages it covers entirely.
 */
extern void lkpi_page_forget_foreign(const void *addr, size_t len);

/*
 * kfree()/vfree() of a page-aligned address: if it is a vmap() address,
 * unmaps it like vunmap() and returns true.
 */
extern bool lkpi_vmap_free(const void *addr);

/* sg_free_table(): drops DMA mappings still made on the list (with a warning). */
extern void lkpi_dma_sgl_release(struct scatterlist *sgl);

/* Global state of lkpi_dma.c and lkpi_io.c, set up by lkpi_page_init(). */
extern void lkpi_dma_init(void);
extern void lkpi_dma_fini(void);
extern void lkpi_io_init(void);
extern void lkpi_io_fini(void);

#endif	/* _LKPI_MEM_H */
