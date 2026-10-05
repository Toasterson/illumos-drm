/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's bsearch() (src/lkpi_misc.c). */
#ifndef _LINUX_BSEARCH_H
#define	_LINUX_BSEARCH_H
#include <linux/types.h>
extern void *lkpi_bsearch(const void *key, const void *base, size_t num,
    size_t size, int (*cmp)(const void *key, const void *elt));
#define	bsearch(k, b, n, s, c)	lkpi_bsearch(k, b, n, s, c)
#endif
