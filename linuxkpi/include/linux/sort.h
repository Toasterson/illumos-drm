/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's sort(): a heap sort (src/lkpi_misc.c). */
#ifndef _LINUX_SORT_H
#define	_LINUX_SORT_H
#include <linux/types.h>
extern void sort(void *base, size_t num, size_t size,
    int (*cmp)(const void *, const void *), void (*swap)(void *, void *, int));
#define	sort_r(b, n, s, c, sw, p)	sort(b, n, s, (void *)(c), sw)
#endif
