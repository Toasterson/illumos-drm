/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's page-attribute changes. Not supported on illumos (no DDI
 * interface changes the caching of kernel pages): set_pages_*() and
 * set_memory_*() return 0 without changing anything. The layer's GPU
 * memory is write-back and drivers flush with clflush (i915 does on
 * non-LLC parts); uncached or write-combining CPU access goes through
 * vmap() with a caching mode instead.
 */
#ifndef _ASM_SET_MEMORY_H
#define	_ASM_SET_MEMORY_H
#define	set_pages_wb(p, n)		((void)(p), (void)(n), 0)
#define	set_pages_uc(p, n)		((void)(p), (void)(n), 0)
#define	set_pages_wc(p, n)		((void)(p), (void)(n), 0)
#define	set_pages_array_wb(p, n)	((void)(p), (void)(n), 0)
#define	set_pages_array_uc(p, n)	((void)(p), (void)(n), 0)
#define	set_pages_array_wc(p, n)	((void)(p), (void)(n), 0)
#define	set_memory_wb(a, n)		((void)(a), (void)(n), 0)
#define	set_memory_uc(a, n)		((void)(a), (void)(n), 0)
#define	set_memory_wc(a, n)		((void)(a), (void)(n), 0)
#endif
