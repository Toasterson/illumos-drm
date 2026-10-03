/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Page-table types for apply_to_page_range() (not supported). */
#ifndef _LINUX_PGTABLE_H
#define	_LINUX_PGTABLE_H
#include <asm/pgtable_types.h>
typedef struct { unsigned long pte; } pte_t;
#define	pte_val(x)	((x).pte)
#define	set_pte_at(mm, addr, ptep, pte)	((void)(mm), (void)(addr), (void)(ptep), (void)(pte))
#define	pfn_pte(pfn, prot)	((pte_t){ ((pfn) << PAGE_SHIFT) | pgprot_val(prot) })
#define	pte_mkspecial(pte)	(pte)
#define	ptep_get(p)		(*(p))
#endif
