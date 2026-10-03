/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's x86 cache maintenance (arch/x86/include/asm/cacheflush.h,
 * special_insns.h; GPL-2.0; reimplemented). clflush/clflushopt flush one
 * cache line; clflush_cache_range() flushes a range with fences; wbinvd
 * writes back the calling CPU's caches.
 */
#ifndef _ASM_CACHEFLUSH_H
#define	_ASM_CACHEFLUSH_H

#include <linux/types.h>
#include <asm/barrier.h>

static inline void
clflush(volatile void *p)
{
	__asm__ __volatile__("clflush %0" : "+m" (*(volatile char *)p));
}

static inline void
clflushopt(volatile void *p)
{
	clflush(p);
}

static inline void
wbinvd(void)
{
	__asm__ __volatile__("wbinvd" ::: "memory");
}

static inline void
clflush_cache_range(void *vaddr, unsigned int size)
{
	unsigned long p = (unsigned long)vaddr & ~((unsigned long)L1_CACHE_BYTES - 1);
	unsigned long end = (unsigned long)vaddr + size;

	mb();
	for (; p < end; p += L1_CACHE_BYTES)
		clflush((void *)p);
	mb();
}

#define	flush_cache_all()		lkpi_wbinvd_all()
#define	flush_cache_range(v, s, e)	((void)(v))
extern void lkpi_wbinvd_all(void);

#endif
