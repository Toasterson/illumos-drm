/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The page flags the layer keeps (Linux's include/linux/page-flags.h,
 * GPL-2.0, as interface reference): dirty and reserved; the others read
 * as clear.
 */
#ifndef _LINUX_PAGE_FLAGS_H
#define	_LINUX_PAGE_FLAGS_H

#define	LKPI_PG_dirty		0
#define	LKPI_PG_reserved	1
#define	PageDirty(p)		test_bit(LKPI_PG_dirty, &(p)->flags)
#define	SetPageDirty(p)		set_bit(LKPI_PG_dirty, &(p)->flags)
#define	ClearPageDirty(p)	clear_bit(LKPI_PG_dirty, &(p)->flags)
#define	PageReserved(p)		test_bit(LKPI_PG_reserved, &(p)->flags)
#define	SetPageReserved(p)	set_bit(LKPI_PG_reserved, &(p)->flags)
#define	ClearPageReserved(p)	clear_bit(LKPI_PG_reserved, &(p)->flags)
#define	folio_test_dirty(f)	PageDirty(&(f)->page)
#define	folio_clear_dirty(f)	ClearPageDirty(&(f)->page)
#define	folio_test_writeback(f)	false
#define	folio_test_swapcache(f)	false
#define	folio_test_lru(f)	false
#define	folio_test_locked(f)	false
#define	folio_test_uptodate(f)	true
#define	PageHighMem(p)		0
#define	PageCompound(p)		0
#define	PageSwapCache(p)	0
#define	PageTransHuge(p)	0
#define	PageLRU(p)		0
#define	folio_lock(f)		((void)(f))
#define	folio_unlock(f)		((void)(f))
#define	folio_trylock(f)	true
#define	lock_page(p)		((void)(p))
#define	unlock_page(p)		((void)(p))
#define	folio_wait_writeback(f)	((void)(f))
#define	folio_clear_reclaim(f)	((void)(f))
#define	folio_set_reclaim(f)	((void)(f))

#endif
