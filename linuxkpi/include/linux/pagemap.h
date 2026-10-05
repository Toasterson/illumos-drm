/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's page-cache interface for the layer's shmem files (include/
 * linux/pagemap.h, GPL-2.0; reimplemented). Pages of a shmem file are
 * created on first access (src/lkpi_shmem.c) and live until the file is
 * truncated or released; there is no page cache or writeback.
 */
#ifndef _LINUX_PAGEMAP_H
#define	_LINUX_PAGEMAP_H

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/highmem.h>

extern struct folio *lkpi_shmem_read_folio(struct address_space *mapping,
    pgoff_t index, gfp_t gfp);
extern void invalidate_mapping_pages(struct address_space *mapping,
    pgoff_t start, pgoff_t end);

#define	read_mapping_folio(m, idx, f)	lkpi_shmem_read_folio((m), (idx), GFP_KERNEL)
#define	read_mapping_page(m, idx, f) \
	({ struct folio *__f = lkpi_shmem_read_folio((m), (idx), GFP_KERNEL); \
	IS_ERR(__f) ? ERR_CAST(__f) : &__f->page; })
#define	mapping_set_error(m, e)		((void)(m), (void)(e))
#define	folio_release_kmap(f, a)	folio_put(f)
#define	page_cache_release(p)		put_page(p)

#endif
