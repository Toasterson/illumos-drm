/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's memory shrinkers (include/linux/shrinker.h, GPL-2.0;
 * reimplemented in src/lkpi_shrinker.c). illumos has no shrinker chain;
 * when the kernel reaps kmem caches (memory runs low), the reclaim
 * callback of a kmem cache the layer creates for that purpose
 * (kmem_cache_create(9F)) queues a shrink pass on the layer's own task
 * queue: at most one pass at a time and, from kmem, at most one a
 * second. A pass calls each registered shrinker's count_objects() and
 * then one scan_objects() with nr_to_scan = min(count, batch)
 * (SHRINK_BATCH, 128, by default), in thread context, with
 * __GFP_DIRECT_RECLAIM unset (shrinkers must not block on allocation).
 * There is no swap. shrinker_free() waits for a scan in progress.
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
	uint64_t lkpi_pass;		/* the last pass that saw it */
	bool lkpi_scanning;		/* a pass is calling it */
	char lkpi_name[32];
};

extern struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
    __printf(2, 3);
extern void shrinker_register(struct shrinker *shrinker);
extern void shrinker_free(struct shrinker *shrinker);
/*
 * Layer interface: queue a shrink pass unless one is queued or running.
 * kmem's reclaim callback passes now = false (rate-limited); the
 * selftest passes true to run one regardless of the last pass's time.
 */
extern void lkpi_shrink_request(bool now);

#endif
