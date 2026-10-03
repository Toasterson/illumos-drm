/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's page protections, reduced to the caching mode the layer passes
 * to the illumos HAT when it maps pages (vmap, io mappings): write-back,
 * write-combining or uncached.
 */
#ifndef _ASM_PGTABLE_TYPES_H
#define	_ASM_PGTABLE_TYPES_H

typedef struct {
	unsigned long pgprot;
} pgprot_t;

#define	__pgprot(x)		((pgprot_t) { (x) })
#define	pgprot_val(x)		((x).pgprot)
#define	LKPI_PROT_WB		0x0UL
#define	LKPI_PROT_WC		0x1UL
#define	LKPI_PROT_UC		0x2UL
#define	LKPI_PROT_CACHE_MASK	0x3UL
#define	PAGE_KERNEL		__pgprot(LKPI_PROT_WB)
#define	PAGE_KERNEL_IO		__pgprot(LKPI_PROT_UC)
#define	PAGE_KERNEL_NOCACHE	__pgprot(LKPI_PROT_UC)
#define	PAGE_KERNEL_RO		__pgprot(LKPI_PROT_WB)
#define	PAGE_SHARED		__pgprot(LKPI_PROT_WB)
#define	pgprot_writecombine(p)	__pgprot((pgprot_val(p) & ~LKPI_PROT_CACHE_MASK) | LKPI_PROT_WC)
#define	pgprot_noncached(p)	__pgprot((pgprot_val(p) & ~LKPI_PROT_CACHE_MASK) | LKPI_PROT_UC)
#define	pgprot_decrypted(p)	(p)
#define	pgprot_modify(o, n)	(n)
#define	_PAGE_CACHE_MASK	LKPI_PROT_CACHE_MASK
#define	cachemode2protval(m)	(m)
enum page_cache_mode {
	_PAGE_CACHE_MODE_WB = LKPI_PROT_WB,
	_PAGE_CACHE_MODE_WC = LKPI_PROT_WC,
	_PAGE_CACHE_MODE_UC_MINUS = LKPI_PROT_UC,
	_PAGE_CACHE_MODE_UC = LKPI_PROT_UC,
	_PAGE_CACHE_MODE_WT = LKPI_PROT_UC,
	_PAGE_CACHE_MODE_WP = LKPI_PROT_UC
};

#endif
