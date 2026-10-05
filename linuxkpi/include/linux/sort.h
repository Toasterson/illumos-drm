/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 The illumos-drm authors. Linux's sort() and sort_r():
 * an in-place heap sort (src/lkpi_misc.c), not stable; a NULL swap
 * function swaps bytes. The *_nonatomic forms are the same.
 */
#ifndef _LINUX_SORT_H
#define	_LINUX_SORT_H
#include <linux/types.h>
typedef void (*swap_r_func_t)(void *a, void *b, int size, const void *priv);
typedef void (*swap_func_t)(void *a, void *b, int size);
typedef int (*cmp_r_func_t)(const void *a, const void *b, const void *priv);
typedef int (*cmp_func_t)(const void *a, const void *b);
#define	cmp_int(l, r)	(((l) > (r)) - ((l) < (r)))
extern void sort(void *base, size_t num, size_t size, cmp_func_t cmp,
    swap_func_t swap);
extern void sort_r(void *base, size_t num, size_t size, cmp_r_func_t cmp,
    swap_r_func_t swap, const void *priv);
#define	sort_nonatomic(b, n, s, c, sw)		sort(b, n, s, c, sw)
#define	sort_r_nonatomic(b, n, s, c, sw, p)	sort_r(b, n, s, c, sw, p)
#endif
