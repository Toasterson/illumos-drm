/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Stand-alone wound/wait mutexes: every struct ww_mutex that is not the
 * lock of a reservation object (DRM's mode-setting locks of class
 * crtc_ww_class, struct drm_modeset_lock). The lock embedded in a
 * dma_resv is the DRM core's and is taken through the core's transactions
 * (src/lkpi_resv.c, which dispatches ww_mutex_*() to the functions here
 * for the locks ww_mutex_init() marked as stand-alone).
 *
 * A stand-alone lock is marked with lkpi_ww_standalone_class in
 * ww_class; its holder is in ctx (the holder's acquire context, or
 * LKPI_WW_NOCTX for a holder without one; NULL when free). The state is
 * changed under the mutex of one of LKPI_WW_BUCKETS hashed buckets, and
 * waiters sleep on the bucket's condition variable, so the lock needs no
 * storage beyond Linux's struct ww_mutex.
 *
 * Deadlocks between transactions are avoided as wait-die (also for
 * wound-wait classes; DRM's users back off on -EDEADLK either way): a
 * transaction that already holds locks and is younger (larger stamp)
 * than the holder of the lock it wants gets -EDEADLK instead of waiting;
 * ww_mutex_lock_slow() (the transaction holds nothing) waits without the
 * check. Stamps come from the core's counter (drm_illumos_ww_stamp(), in
 * ww_acquire_init()), shared with reservation transactions, so a cycle
 * that mixes both kinds of lock is broken the same way.
 *
 * Fairness differs from Linux: there is no queue of waiters. Linux keeps
 * a lock's waiters in stamp order and hands the lock to the oldest; here
 * an unlock wakes every waiter of the bucket and the first to run takes
 * the lock, so a waiter may lose the race repeatedly (no starvation
 * bound), and an older transaction is not preferred. Correctness does
 * not depend on the order (the die rule needs only the holder's stamp),
 * and DRM's mode-setting locks are held briefly by few threads.
 *
 * Thread context only (the locks may sleep).
 */

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/string.h>
#include <linux/ww_mutex.h>

#include "lkpi_ww_mutex.h"

static struct ww_class lkpi_ww_standalone_class =
    __WW_CLASS_INITIALIZER(lkpi_ww_standalone, 1);

#define	LKPI_WW_NOCTX	((void *)(uintptr_t)1)
#define	LKPI_WW_BUCKETS	64

static struct lkpi_ww_bucket {
	kmutex_t wb_lock;
	kcondvar_t wb_cv;
} lkpi_ww_buckets[LKPI_WW_BUCKETS];

int
lkpi_ww_init(void)
{
	int i;

	for (i = 0; i < LKPI_WW_BUCKETS; i++) {
		(mutex_init)(&lkpi_ww_buckets[i].wb_lock, NULL, MUTEX_DRIVER,
		    NULL);
		cv_init(&lkpi_ww_buckets[i].wb_cv, NULL, CV_DRIVER, NULL);
	}
	return (0);
}

void
lkpi_ww_fini(void)
{
	int i;

	for (i = 0; i < LKPI_WW_BUCKETS; i++) {
		cv_destroy(&lkpi_ww_buckets[i].wb_cv);
		(mutex_destroy)(&lkpi_ww_buckets[i].wb_lock);
	}
}

static struct lkpi_ww_bucket *
ww_bucket(const struct ww_mutex *lock)
{
	uintptr_t h = (uintptr_t)lock;

	h ^= h >> 6;
	h ^= h >> 12;
	return (&lkpi_ww_buckets[h % LKPI_WW_BUCKETS]);
}

void
lkpi_ww_mutex_init(struct ww_mutex *lock, struct ww_class *c)
{
	(void) c;
	memset(lock, 0, sizeof (*lock));
	lock->ww_class = &lkpi_ww_standalone_class;
}

bool
lkpi_ww_standalone(const struct ww_mutex *lock)
{
	return (lock->ww_class == &lkpi_ww_standalone_class);
}

/*
 * Takes a stand-alone lock: 0, -EALREADY if ctx holds it, -EDEADLK if ctx
 * must back off (not with `slow`), -EINTR if `intr` and a signal came.
 */
int
lkpi_ww_sa_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx, bool slow,
    bool intr)
{
	struct lkpi_ww_bucket *b = ww_bucket(lock);
	struct ww_acquire_ctx *holder;
	int ret = 0;

	mutex_enter(&b->wb_lock);
	for (;;) {
		holder = lock->ctx;
		if (holder == NULL)
			break;
		if (ctx != NULL && holder == ctx) {
			ret = -EALREADY;
			goto out;
		}
		if (ctx != NULL && !slow && holder != LKPI_WW_NOCTX &&
		    ctx->acquired > 0 && ctx->stamp > holder->stamp) {
			ctx->contending_lock = lock;
			ret = -EDEADLK;
			goto out;
		}
		if (intr) {
			if (cv_wait_sig(&b->wb_cv, &b->wb_lock) == 0) {
				ret = -EINTR;
				goto out;
			}
		} else {
			cv_wait(&b->wb_cv, &b->wb_lock);
		}
	}
	lock->ctx = ctx != NULL ? (void *)ctx : LKPI_WW_NOCTX;
	if (ctx != NULL) {
		ctx->acquired++;
		ctx->contending_lock = NULL;
	}
out:
	mutex_exit(&b->wb_lock);
	return (ret);
}

/* 1 if the lock was free and is now held (by ctx, if any), else 0. */
int
lkpi_ww_sa_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct lkpi_ww_bucket *b = ww_bucket(lock);
	int got = 0;

	mutex_enter(&b->wb_lock);
	if (lock->ctx == NULL) {
		lock->ctx = ctx != NULL ? (void *)ctx : LKPI_WW_NOCTX;
		if (ctx != NULL)
			ctx->acquired++;
		got = 1;
	}
	mutex_exit(&b->wb_lock);
	return (got);
}

/* Releases the lock and wakes its waiters; a free lock is refused. */
void
lkpi_ww_sa_unlock(struct ww_mutex *lock)
{
	struct lkpi_ww_bucket *b = ww_bucket(lock);
	struct ww_acquire_ctx *holder;

	mutex_enter(&b->wb_lock);
	holder = lock->ctx;
	if (WARN(holder == NULL, "ww_mutex_unlock of a free lock")) {
		mutex_exit(&b->wb_lock);
		return;
	}
	lock->ctx = NULL;
	if (holder != LKPI_WW_NOCTX && !WARN_ON(holder->acquired == 0))
		holder->acquired--;
	cv_broadcast(&b->wb_cv);
	mutex_exit(&b->wb_lock);
}

bool
lkpi_ww_sa_is_locked(const struct ww_mutex *lock)
{
	return (READ_ONCE(lock->ctx) != NULL);
}
