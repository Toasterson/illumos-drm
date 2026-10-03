/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kmap() family (include/linux/highmem.h, GPL-2.0;
 * reimplemented). x86-64 has no high memory and every layer page has a
 * kernel address, so mapping a page returns that address and unmapping
 * does nothing.
 */
#ifndef _LINUX_HIGHMEM_H
#define	_LINUX_HIGHMEM_H

#include <linux/mm.h>
#include <linux/string.h>
#include <asm/cacheflush.h>

#define	kmap(page)			page_address(page)
#define	kunmap(page)			((void)(page))
#define	kmap_atomic(page)		page_address(page)
#define	kunmap_atomic(addr)		((void)(addr))
#define	kmap_local_page(page)		page_address(page)
#define	kmap_local_page_try_from_panic(page)	page_address(page)
#define	kmap_local_folio(f, off)	((void *)((char *)folio_address(f) + (off)))
#define	kunmap_local(addr)		((void)(addr))
#define	kmap_atomic_prot(page, prot)	page_address(page)
#define	kmap_local_page_prot(page, prot)	page_address(page)

static inline void
clear_page(void *page)
{
	memset(page, 0, PAGE_SIZE);
}

static inline void
clear_highpage(struct lx_page *page)
{
	clear_page(page_address(page));
}

static inline void
copy_highpage(struct lx_page *to, struct lx_page *from)
{
	memcpy(page_address(to), page_address(from), PAGE_SIZE);
}

static inline void
memcpy_from_page(char *to, struct lx_page *page, size_t offset, size_t len)
{
	memcpy(to, (char *)page_address(page) + offset, len);
}

static inline void
memcpy_to_page(struct lx_page *page, size_t offset, const char *from,
    size_t len)
{
	memcpy((char *)page_address(page) + offset, from, len);
}

static inline void
memzero_page(struct lx_page *page, size_t offset, size_t len)
{
	memset((char *)page_address(page) + offset, 0, len);
}

#define	folio_zero_range(f, off, len)	memzero_page(&(f)->page, off, len)
#define	flush_dcache_page(p)		((void)(p))
#define	flush_kernel_vmap_range(a, s)	((void)(a))
#define	invalidate_kernel_vmap_range(a, s)	((void)(a))

#endif
