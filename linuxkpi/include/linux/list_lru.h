/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's list_lru, reduced to the structure TTM's pool embeds (the TTM
 * pool is not used on illumos).
 */
#ifndef _LINUX_LIST_LRU_H
#define	_LINUX_LIST_LRU_H
#include <linux/list.h>
#include <linux/spinlock.h>
struct list_lru {
	spinlock_t lock;
	struct list_head list;
	long nr_items;
};
#endif
