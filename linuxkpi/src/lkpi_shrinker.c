/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Memory shrinkers over kmem reclaim callbacks (the interface of Linux's
 * <linux/shrinker.h>; independent implementation).
 *
 * illumos has no shrinker chain. When memory runs low, kmem_reap() calls
 * the reclaim function of every kmem cache from its task queue, so each
 * registered shrinker gets a kmem cache of its own (never allocated from)
 * whose reclaim function asks it to free memory: count_objects() and, if
 * there is something, one scan_objects() of nr_to_scan = batch
 * (SHRINK_BATCH by default) objects with a gfp mask that does not allow
 * direct reclaim. shrinker_free() destroys the cache;
 * kmem_cache_destroy() waits for reclaim tasks in progress, so the
 * shrinker is not called after it returns.
 */

#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/list.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

static kmutex_t lkpi_shrinker_lock;
static struct list_head lkpi_shrinkers;
static unsigned int lkpi_shrinker_seq;

static void
lkpi_shrinker_reclaim(void *arg)
{
	struct shrinker *s = arg;
	struct shrink_control sc = {
		.gfp_mask = GFP_NOWAIT | __GFP_NOWARN,
		.nid = 0
	};
	unsigned long count, batch;

	if (!(s->flags & SHRINKER_REGISTERED) || s->count_objects == NULL ||
	    s->scan_objects == NULL)
		return;
	count = s->count_objects(s, &sc);
	if (count == 0 || count == SHRINK_EMPTY)
		return;
	batch = s->batch > 0 ? (unsigned long)s->batch : SHRINK_BATCH;
	sc.nr_to_scan = MIN(count, batch);
	sc.nr_scanned = sc.nr_to_scan;
	(void) s->scan_objects(s, &sc);
}

struct shrinker *
shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	struct shrinker *s;
	va_list ap;

	s = kmem_zalloc(sizeof (*s), KM_SLEEP);
	va_start(ap, fmt);
	(void) vsnprintf(s->lkpi_name, sizeof (s->lkpi_name), fmt, ap);
	va_end(ap);
	s->flags = flags | SHRINKER_ALLOCATED;
	s->seeks = DEFAULT_SEEKS;
	INIT_LIST_HEAD(&s->lkpi_list);
	return (s);
}

void
shrinker_register(struct shrinker *s)
{
	char name[32];
	unsigned int n;

	if (s == NULL || (s->flags & SHRINKER_REGISTERED))
		return;
	mutex_enter(&lkpi_shrinker_lock);
	n = ++lkpi_shrinker_seq;
	mutex_exit(&lkpi_shrinker_lock);
	/* kmem cache names are C identifiers; the shrinker's name is not. */
	(void) snprintf(name, sizeof (name), "lkpi_shrinker_%u", n);
	s->lkpi_cache = (kmem_cache_create)(name, 8, 0, NULL, NULL,
	    lkpi_shrinker_reclaim, s, NULL, 0);
	mutex_enter(&lkpi_shrinker_lock);
	list_add_tail(&s->lkpi_list, &lkpi_shrinkers);
	s->flags |= SHRINKER_REGISTERED;
	mutex_exit(&lkpi_shrinker_lock);
}

static void
lkpi_shrinker_unregister(struct shrinker *s)
{
	mutex_enter(&lkpi_shrinker_lock);
	s->flags &= ~SHRINKER_REGISTERED;
	list_del_init(&s->lkpi_list);
	mutex_exit(&lkpi_shrinker_lock);
	(kmem_cache_destroy)(s->lkpi_cache);
	s->lkpi_cache = NULL;
}

void
shrinker_free(struct shrinker *s)
{
	if (s == NULL)
		return;
	if (s->lkpi_cache != NULL)
		lkpi_shrinker_unregister(s);
	kmem_free(s, sizeof (*s));
}

int
lkpi_shrinker_init(void)
{
	(mutex_init)(&lkpi_shrinker_lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&lkpi_shrinkers);
	return (0);
}

/* Shrinkers the driver did not free stop being called. */
void
lkpi_shrinker_fini(void)
{
	struct shrinker *s;
	unsigned long n = 0;

	while (!list_empty(&lkpi_shrinkers)) {
		s = list_first_entry(&lkpi_shrinkers, struct shrinker,
		    lkpi_list);
		lkpi_shrinker_unregister(s);
		n++;
	}
	if (n != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu shrinkers left", n);
	(mutex_destroy)(&lkpi_shrinker_lock);
}
