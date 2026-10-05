/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Memory shrinkers over the kmem reclaim callback (the interface of
 * Linux's <linux/shrinker.h>; independent implementation).
 *
 * illumos has no shrinker chain. When memory runs low, kmem_reap() calls
 * the reclaim function of every kmem cache from its task queue. The layer
 * creates one kmem cache (never allocated from) whose reclaim function
 * only asks for a shrink pass (lkpi_shrink_request()); the pass runs on
 * the layer's own single-thread task queue, never in kmem's reap thread,
 * so a scan that blocks (i915 takes vm->mutex and object locks) delays
 * only the next pass, not the reaping of every other kmem cache.
 *
 * At most one pass is queued or running at a time; a request during a
 * pass is dropped, and a request from kmem within LKPI_SHRINK_INTERVAL of
 * the previous pass's start is dropped too (kmem asks again while memory
 * stays low). A pass calls, for each registered shrinker in turn,
 * count_objects() and, if there is something, one scan_objects() of
 * nr_to_scan = min(count, batch) (SHRINK_BATCH by default) with a gfp mask
 * that does not allow direct reclaim.
 *
 * There is no swap (get_nr_swap_pages() is 0): i915's scan only frees the
 * pages of purgeable (I915_MADV_DONTNEED) objects and skips the others
 * (can_release_pages()), so a pass releases what userspace marked
 * disposable and leaves the bindings and DMA mappings of other objects
 * alone; it cannot page anything out.
 *
 * shrinker_free() unlinks the shrinker and waits for a scan of it that
 * is in progress; it is not called afterwards.
 */

#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/list.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/* The minimum time between passes kmem asks for: one second. */
#define	LKPI_SHRINK_INTERVAL_US	1000000

static kmutex_t lkpi_shrinker_lock;	/* the list, lkpi_scanning */
static kcondvar_t lkpi_shrinker_cv;	/* a scan has returned */
static struct list_head lkpi_shrinkers;
static kmem_cache_t *lkpi_shrinker_cache;	/* the reclaim hook */
static ddi_taskq_t *lkpi_shrinker_tq;
/* 0: idle; 1: a pass is queued or running (one at a time). */
static volatile uint32_t lkpi_shrink_busy;
static volatile int64_t lkpi_shrink_last;	/* lbolt64 of the last pass */
static uint64_t lkpi_shrink_pass;		/* numbers the passes */

static void
lkpi_shrinker_scan(struct shrinker *s)
{
	struct shrink_control sc = {
		.gfp_mask = GFP_NOWAIT | __GFP_NOWARN,
		.nid = 0
	};
	unsigned long count, batch;

	count = s->count_objects(s, &sc);
	if (count == 0 || count == SHRINK_EMPTY)
		return;
	batch = s->batch > 0 ? (unsigned long)s->batch : SHRINK_BATCH;
	sc.nr_to_scan = MIN(count, batch);
	sc.nr_scanned = sc.nr_to_scan;
	(void) s->scan_objects(s, &sc);
}

/* The task: one pass over the registered shrinkers. */
static void
lkpi_shrink_task(void *arg)
{
	struct shrinker *s, *next;
	uint64_t pass;

	mutex_enter(&lkpi_shrinker_lock);
	pass = ++lkpi_shrink_pass;
	for (;;) {
		/*
		 * The list may change while a scan runs unlocked: look for the
		 * first shrinker this pass has not seen, each time.
		 */
		next = NULL;
		list_for_each_entry(s, &lkpi_shrinkers, lkpi_list) {
			if (s->lkpi_pass != pass) {
				next = s;
				break;
			}
		}
		if (next == NULL)
			break;
		next->lkpi_pass = pass;
		if (next->count_objects == NULL || next->scan_objects == NULL)
			continue;
		next->lkpi_scanning = true;
		mutex_exit(&lkpi_shrinker_lock);
		lkpi_shrinker_scan(next);
		mutex_enter(&lkpi_shrinker_lock);
		next->lkpi_scanning = false;
		cv_broadcast(&lkpi_shrinker_cv);
	}
	mutex_exit(&lkpi_shrinker_lock);
	__atomic_store_n(&lkpi_shrink_busy, 0, __ATOMIC_RELEASE);
}

void
lkpi_shrink_request(bool now)
{
	uint32_t idle = 0;
	int64_t t = ddi_get_lbolt64();

	if (lkpi_shrinker_tq == NULL)
		return;
	if (!now && lkpi_shrink_last != 0 &&
	    t - lkpi_shrink_last < drv_usectohz(LKPI_SHRINK_INTERVAL_US))
		return;
	if (!__atomic_compare_exchange_n(&lkpi_shrink_busy, &idle, 1, false,
	    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return;
	lkpi_shrink_last = t;
	if (ddi_taskq_dispatch(lkpi_shrinker_tq, lkpi_shrink_task, NULL,
	    DDI_NOSLEEP) != DDI_SUCCESS)
		__atomic_store_n(&lkpi_shrink_busy, 0, __ATOMIC_RELEASE);
}

/* kmem's reclaim callback, from its reap task queue: must not block. */
static void
lkpi_shrinker_reclaim(void *arg)
{
	lkpi_shrink_request(false);
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
	s->flags = (flags & ~SHRINKER_REGISTERED) | SHRINKER_ALLOCATED;
	s->seeks = DEFAULT_SEEKS;
	INIT_LIST_HEAD(&s->lkpi_list);
	return (s);
}

void
shrinker_register(struct shrinker *s)
{
	if (s == NULL)
		return;
	mutex_enter(&lkpi_shrinker_lock);
	if (!(s->flags & SHRINKER_REGISTERED)) {
		s->flags |= SHRINKER_REGISTERED;
		list_add_tail(&s->lkpi_list, &lkpi_shrinkers);
	}
	mutex_exit(&lkpi_shrinker_lock);
}

/* Unlinks s and waits for a scan of it in progress; lock held. */
static void
lkpi_shrinker_unlink(struct shrinker *s)
{
	ASSERT(MUTEX_HELD(&lkpi_shrinker_lock));
	if (s->flags & SHRINKER_REGISTERED) {
		s->flags &= ~SHRINKER_REGISTERED;
		list_del_init(&s->lkpi_list);
	}
	while (s->lkpi_scanning)
		cv_wait(&lkpi_shrinker_cv, &lkpi_shrinker_lock);
}

void
shrinker_free(struct shrinker *s)
{
	if (s == NULL)
		return;
	mutex_enter(&lkpi_shrinker_lock);
	lkpi_shrinker_unlink(s);
	mutex_exit(&lkpi_shrinker_lock);
	kmem_free(s, sizeof (*s));
}

int
lkpi_shrinker_init(void)
{
	(mutex_init)(&lkpi_shrinker_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&lkpi_shrinker_cv, NULL, CV_DRIVER, NULL);
	INIT_LIST_HEAD(&lkpi_shrinkers);
	lkpi_shrink_busy = 0;
	lkpi_shrink_last = 0;
	lkpi_shrinker_tq = ddi_taskq_create(NULL, "lkpi_shrinker", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (lkpi_shrinker_tq == NULL) {
		cv_destroy(&lkpi_shrinker_cv);
		(mutex_destroy)(&lkpi_shrinker_lock);
		return (ENOMEM);
	}
	/* Created last: its reclaim callback may run at once. */
	lkpi_shrinker_cache = (kmem_cache_create)("lkpi_shrinker", 8, 0, NULL,
	    NULL, lkpi_shrinker_reclaim, NULL, NULL, 0);
	return (0);
}

/* Shrinkers the driver did not free stop being called. */
void
lkpi_shrinker_fini(void)
{
	ddi_taskq_t *tq = lkpi_shrinker_tq;
	struct shrinker *s;
	unsigned long n = 0;

	/* No more requests from kmem; then wait for a queued pass. */
	(kmem_cache_destroy)(lkpi_shrinker_cache);
	lkpi_shrinker_cache = NULL;
	lkpi_shrinker_tq = NULL;
	ddi_taskq_destroy(tq);

	mutex_enter(&lkpi_shrinker_lock);
	while (!list_empty(&lkpi_shrinkers)) {
		s = list_first_entry(&lkpi_shrinkers, struct shrinker,
		    lkpi_list);
		lkpi_shrinker_unlink(s);
		n++;
	}
	mutex_exit(&lkpi_shrinker_lock);
	if (n != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu shrinkers left", n);
	cv_destroy(&lkpi_shrinker_cv);
	(mutex_destroy)(&lkpi_shrinker_lock);
}
