/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Stub of Linux's include/linux/scatterlist.h for the carried helpers
 * (carried/): drm_cache.h includes it for the prototypes of
 * drm_clflush_pages() and drm_clflush_sg(), which take a struct page and
 * a struct sg_table. Only the incomplete types are declared; the
 * functions themselves are not built until R5c's page and scatterlist
 * models exist (carried/drm_cache/patches/0001-*).
 *
 * Note for R5c: the illumos kernel has a struct page of its own (page_t,
 * vm/page.h); a declaration here names the same tag wherever that header
 * is in scope.
 *
 * To be reconciled with R5c: R5c's scatterlist.h replaces this stub.
 */

#ifndef _LINUXKPI_LINUX_SCATTERLIST_H
#define	_LINUXKPI_LINUX_SCATTERLIST_H

#include <linux/types.h>

struct page;
struct sg_table;

#endif	/* _LINUXKPI_LINUX_SCATTERLIST_H */
