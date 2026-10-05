/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's memory shrinkers (include/linux/shrinker.h, GPL-2.0;
 * reimplemented in src/lkpi_shrinker.c). illumos has no shrinker chain;
 * the layer asks registered shrinkers to free memory from the reclaim
 * callback of a kmem cache it creates for that purpose
 * (kmem_cache_create(9F)'s reclaim function, which the kernel calls when
 * memory runs low): count_objects() and then scan_objects() with
 * nr_to_scan = SHRINK_BATCH (128) per call, in thread context, with
 * __GFP_DIRECT_RECLAIM unset (shrinkers must not block on allocation).
 */
#ifndef _LINUX_SHRINKER_H
#define	_LINUX_SHRINKER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/gfp.h>

struct shrink_control {
	gfp_t gfp_mask;
	int nid;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
	void *memcg;
};

#define	SHRINK_STOP		(~0UL)
#define	SHRINK_EMPTY		(~0UL - 1)
#define	DEFAULT_SEEKS		2
#define	SHRINKER_REGISTERED	(1 << 0)
#define	SHRINKER_ALLOCATED	(1 << 1)
#define	SHRINKER_NUMA_AWARE	(1 << 2)
#define	SHRINKER_MEMCG_AWARE	(1 << 3)
#define	SHRINKER_NONSLAB	(1 << 4)
#define	SHRINK_BATCH		128

struct shrinker {
	unsigned long (*count_objects)(struct shrinker *, struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *sc);
	long batch;
	int seeks;
	unsigned int flags;
	void *private_data;
	struct list_head lkpi_list;
	void *lkpi_cache;
	char lkpi_name[32];
};

extern struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
    __printf(2, 3);
extern void shrinker_register(struct shrinker *shrinker);
extern void shrinker_free(struct shrinker *shrinker);

#endif
