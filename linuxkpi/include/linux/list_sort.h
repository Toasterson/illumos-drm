/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. list_sort() (src/lkpi_misc.c: a stable merge sort). */
#ifndef _LINUX_LIST_SORT_H
#define	_LINUX_LIST_SORT_H
#include <linux/list.h>
typedef int (*list_cmp_func_t)(void *, const struct list_head *,
    const struct list_head *);
#endif
