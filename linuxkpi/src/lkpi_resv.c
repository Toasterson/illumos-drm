/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's reservation-object helpers and wound/wait transactions over the
 * DRM facade (facade 1.1). The reservations, their locks and fence lists
 * are the DRM core's; this file keeps the C side of a transaction (struct
 * ww_acquire_ctx: the core's stamp and the number of locks held) and
 * builds Linux's iterators on snapshots of the fence lists.
 *
 * Lock ownership: a reservation locked by a transaction records the
 * transaction in its public placeholder lock (dma_resv.lock.ctx), which
 * only the holder reads and writes; dma_resv_unlock() uses it to release
 * through the right core call and to count the transaction down.
 */

#include <linux/dma-resv.h>
#include <linux/dma-fence-array.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "lkpi_internal.h"
#include "lkpi_ww_mutex.h"

static void lkpi_resv_snaps_release(struct dma_resv *obj);

/* The class of reservation locks: one class, the core's. */
struct ww_class reservation_ww_class =
    __WW_CLASS_INITIALIZER(reservation_ww_class, 1);

void
ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class)
{
	memset(ctx, 0, sizeof (*ctx));
	ctx->ww_class = ww_class;
	ctx->is_wait_die = 1;
	/*
	 * Reservation locks are the core's and stand-alone locks the
	 * layer's (below); both use the core's stamps, so one transaction
	 * may take either kind and the die rule holds across them.
	 */
	ctx->stamp = drm_illumos_ww_stamp();
}

void
ww_acquire_done(struct ww_acquire_ctx *ctx)
{
	(void) ctx;
}

void
ww_acquire_fini(struct ww_acquire_ctx *ctx)
{
	WARN(ctx->acquired != 0, "ww_acquire_fini with %u locks held",
	    ctx->acquired);
}

static int
resv_lock(struct dma_resv *obj, struct ww_acquire_ctx *ctx, bool slow,
    bool intr)
{
	int ret;

	if (ctx == NULL) {
		ret = intr ? (dma_resv_lock_interruptible)(obj, NULL) :
		    (dma_resv_lock)(obj, NULL);
		if (ret == 0)
			obj->lock.ctx = NULL;
		return (ret);
	}
	ret = drm_illumos_resv_lock_ctx(obj, ctx->stamp, ctx->acquired, slow,
	    intr);
	if (ret == 0) {
		ctx->acquired++;
		ctx->contending_lock = NULL;
		obj->lock.ctx = ctx;
	} else if (ret == -EDEADLK) {
		ctx->contending_lock = &obj->lock;
	}
	return (ret);
}

int
lkpi_dma_resv_lock(struct dma_resv *obj, struct ww_acquire_ctx *ctx,
    bool intr)
{
	return (resv_lock(obj, ctx, false, intr));
}

int
lkpi_dma_resv_lock_slow(struct dma_resv *obj, struct ww_acquire_ctx *ctx,
    bool intr)
{
	return (resv_lock(obj, ctx, ctx != NULL, intr));
}

void
lkpi_dma_resv_unlock(struct dma_resv *obj)
{
	struct ww_acquire_ctx *ctx = obj->lock.ctx;

	/* Snapshots of locked iterations end with the lock. */
	lkpi_resv_snaps_release(obj);
	if (ctx != NULL) {
		/*
		 * The record goes before the lock: once the core releases it,
		 * the next holder writes its own. The core refuses an unlock
		 * by a thread that does not hold the lock; then the record
		 * and the count are put back as they were.
		 */
		obj->lock.ctx = NULL;
		if (drm_illumos_resv_unlock_ctx(obj, ctx->stamp) != 0) {
			obj->lock.ctx = ctx;
			return;
		}
		WARN_ON(ctx->acquired == 0);
		ctx->acquired--;
	} else {
		(dma_resv_unlock)(obj);
	}
}

bool
lkpi_dma_resv_held(struct dma_resv *obj)
{
	return (drm_illumos_resv_is_locked(obj));
}

/* --- ww_mutex ------------------------------------------------------------ */

/*
 * Two kinds of wound/wait mutex reach these functions. The lock embedded
 * in a reservation object (dma_resv.lock) is the core's: it is taken
 * through the core's transactions above. Every other ww_mutex is
 * stand-alone (marked by ww_mutex_init(): DRM's mode-setting locks) and
 * implemented in src/lkpi_ww_mutex.c.
 */

static struct dma_resv *
ww_resv(struct ww_mutex *lock)
{
	return (container_of(lock, struct dma_resv, lock));
}

int
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (lkpi_ww_standalone(lock))
		return (lkpi_ww_sa_lock(lock, ctx, false, false));
	return (resv_lock(ww_resv(lock), ctx, false, false));
}

int
ww_mutex_lock_interruptible(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx)
{
	if (lkpi_ww_standalone(lock))
		return (lkpi_ww_sa_lock(lock, ctx, false, true));
	return (resv_lock(ww_resv(lock), ctx, false, true));
}

void
ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (lkpi_ww_standalone(lock)) {
		(void) lkpi_ww_sa_lock(lock, ctx, true, false);
		return;
	}
	(void) resv_lock(ww_resv(lock), ctx, ctx != NULL, false);
}

int
ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx)
{
	if (lkpi_ww_standalone(lock))
		return (lkpi_ww_sa_lock(lock, ctx, true, true));
	return (resv_lock(ww_resv(lock), ctx, ctx != NULL, true));
}

int
ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct dma_resv *obj;

	if (lkpi_ww_standalone(lock))
		return (lkpi_ww_sa_trylock(lock, ctx));
	obj = ww_resv(lock);
	if (ctx == NULL) {
		if (!dma_resv_trylock(obj))
			return (0);
		obj->lock.ctx = NULL;
		return (1);
	}
	if (!drm_illumos_resv_trylock_ctx(obj, ctx->stamp))
		return (0);
	ctx->acquired++;
	obj->lock.ctx = ctx;
	return (1);
}

void
ww_mutex_unlock(struct ww_mutex *lock)
{
	if (lkpi_ww_standalone(lock)) {
		lkpi_ww_sa_unlock(lock);
		return;
	}
	lkpi_dma_resv_unlock(ww_resv(lock));
}

bool
ww_mutex_is_locked(struct ww_mutex *lock)
{
	if (lkpi_ww_standalone(lock))
		return (lkpi_ww_sa_is_locked(lock));
	return (drm_illumos_resv_is_locked(ww_resv(lock)));
}

/* --- fence snapshots ------------------------------------------------- */

/*
 * The fences of `obj` with a usage at least as important as `usage`, each
 * with a reference, in a kmalloc'd array, with the usage each has: one
 * core snapshot (drm_illumos_resv_fences(), which gives a driver's own
 * fences back as its structures). `locked` includes signalled fences, as
 * Linux's locked iteration sees every fence of the object.
 */
static int
resv_snapshot(struct dma_resv *obj, enum dma_resv_usage usage, bool locked,
    struct dma_fence ***fencesp, u8 **usagep, unsigned int *countp)
{
	struct dma_fence **fences = NULL;
	u8 *usages = NULL;
	uint32_t want = 8, got = 0, i;
	uint32_t flags = locked ? DRM_ILLUMOS_RESV_SIGNALED : 0;
	int ret;

	*fencesp = NULL;
	*usagep = NULL;
	*countp = 0;
	for (;;) {
		fences = kmalloc_array(want, sizeof (*fences), GFP_KERNEL);
		usages = kmalloc(want, GFP_KERNEL);
		if (fences == NULL || usages == NULL) {
			kfree(fences);
			kfree(usages);
			return (-ENOMEM);
		}
		ret = drm_illumos_resv_fences(obj, usage, flags, fences, usages,
		    want, &got);
		if (ret != 0) {
			kfree(fences);
			kfree(usages);
			return (ret);
		}
		if (got <= want)
			break;
		for (i = 0; i < want; i++)
			dma_fence_put(fences[i]);
		kfree(fences);
		kfree(usages);
		want = got;
	}
	*fencesp = fences;
	*usagep = usages;
	*countp = got;
	return (0);
}

/*
 * Snapshots of locked iterations. Linux's locked iteration
 * (dma_resv_for_each_fence()) holds no references and has no end call, so
 * callers stop at any point and never call dma_resv_iter_end(). A locked
 * snapshot is released when its walk reaches the end, or, for a walk the
 * caller left early, when the object is unlocked; until then it is kept
 * here, per object. The fences it holds stay valid while the lock is held,
 * as on Linux.
 */
struct lkpi_resv_snap {
	struct list_head	rs_link;
	struct dma_resv		*rs_obj;
	kthread_t		*rs_owner;	/* the walking (locking) thread */
	struct dma_fence	**rs_fences;
	u8			*rs_usages;
	unsigned int		rs_count;
};

static DEFINE_MUTEX(lkpi_resv_snaps_lock);
static LIST_HEAD(lkpi_resv_snaps);
/* Registered snapshots: an unlock with none looks at nothing. */
static atomic_t lkpi_resv_nsnaps = ATOMIC_INIT(0);

static void
snap_free(struct lkpi_resv_snap *rs)
{
	unsigned int i;

	for (i = 0; i < rs->rs_count; i++)
		dma_fence_put(rs->rs_fences[i]);
	kfree(rs->rs_fences);
	kfree(rs->rs_usages);
	kfree(rs);
}

static void
lkpi_resv_snaps_release(struct dma_resv *obj)
{
	struct lkpi_resv_snap *rs, *n;
	LIST_HEAD(done);

	if (atomic_read(&lkpi_resv_nsnaps) == 0)
		return;
	mutex_lock(&lkpi_resv_snaps_lock);
	/*
	 * Only the unlocking thread's walks: the lock is that thread's, and
	 * an unlock by another thread (which the core refuses) must not free
	 * the holder's walk.
	 */
	list_for_each_entry_safe(rs, n, &lkpi_resv_snaps, rs_link) {
		if (rs->rs_obj == obj && rs->rs_owner == curthread) {
			list_move(&rs->rs_link, &done);
			atomic_dec(&lkpi_resv_nsnaps);
		}
	}
	mutex_unlock(&lkpi_resv_snaps_lock);
	list_for_each_entry_safe(rs, n, &done, rs_link) {
		list_del(&rs->rs_link);
		snap_free(rs);
	}
}

/* Unload: walks left early on objects never unlocked again. */
void
lkpi_resv_fini(void)
{
	struct lkpi_resv_snap *rs, *n;

	mutex_lock(&lkpi_resv_snaps_lock);
	list_for_each_entry_safe(rs, n, &lkpi_resv_snaps, rs_link) {
		WARN_ONCE(1, "dma_resv: a locked walk outlived its lock");
		list_del(&rs->rs_link);
		atomic_dec(&lkpi_resv_nsnaps);
		snap_free(rs);
	}
	mutex_unlock(&lkpi_resv_snaps_lock);
}

static void
iter_release(struct dma_resv_iter *cursor)
{
	struct lkpi_resv_snap *rs = cursor->lkpi_snap;

	if (rs != NULL) {
		if (cursor->locked) {
			mutex_lock(&lkpi_resv_snaps_lock);
			list_del(&rs->rs_link);
			atomic_dec(&lkpi_resv_nsnaps);
			mutex_unlock(&lkpi_resv_snaps_lock);
		}
		snap_free(rs);
	}
	cursor->lkpi_snap = NULL;
	cursor->fences = NULL;
	cursor->lkpi_usages = NULL;
	cursor->num_fences = 0;
	cursor->fence = NULL;
}

static struct dma_fence *
iter_at(struct dma_resv_iter *cursor)
{
	if (cursor->index >= cursor->num_fences) {
		/* The end of a locked walk releases its snapshot. */
		if (cursor->locked)
			iter_release(cursor);
		cursor->fence = NULL;
		return (NULL);
	}
	cursor->fence = cursor->fences[cursor->index];
	cursor->fence_usage = cursor->lkpi_usages[cursor->index];
	return (cursor->fence);
}

/*
 * A snapshot failure (no memory) cannot be reported through Linux's
 * interface, where an empty walk means "no fences". An unlocked walk then
 * waits for the fences and reports none; a locked walk (which must not
 * wait with the lock held for other users' work any longer than Linux
 * would) waits too: either way the caller sees an idle reservation only
 * once it is idle.
 */
static struct dma_fence *
iter_first(struct dma_resv_iter *cursor, bool locked)
{
	struct lkpi_resv_snap *rs;

	iter_release(cursor);
	cursor->index = 0;
	cursor->is_restarted = true;
	cursor->locked = locked;
	rs = kzalloc(sizeof (*rs), GFP_KERNEL);
	if (rs == NULL || resv_snapshot(cursor->obj, cursor->usage, locked,
	    &rs->rs_fences, &rs->rs_usages, &rs->rs_count) != 0) {
		kfree(rs);
		WARN_ONCE(1, "dma_resv iterator: no memory for a snapshot; "
		    "waiting for the fences instead");
		(void) (dma_resv_wait_timeout)(cursor->obj, cursor->usage,
		    false, MAX_SCHEDULE_TIMEOUT);
		return (NULL);
	}
	rs->rs_obj = cursor->obj;
	cursor->lkpi_snap = rs;
	cursor->fences = rs->rs_fences;
	cursor->lkpi_usages = rs->rs_usages;
	cursor->num_fences = rs->rs_count;
	if (locked) {
		rs->rs_owner = curthread;
		mutex_lock(&lkpi_resv_snaps_lock);
		list_add(&rs->rs_link, &lkpi_resv_snaps);
		atomic_inc(&lkpi_resv_nsnaps);
		mutex_unlock(&lkpi_resv_snaps_lock);
	} else {
		INIT_LIST_HEAD(&rs->rs_link);
	}
	return (iter_at(cursor));
}

struct dma_fence *
dma_resv_iter_first_unlocked(struct dma_resv_iter *cursor)
{
	return (iter_first(cursor, false));
}

struct dma_fence *
dma_resv_iter_next_unlocked(struct dma_resv_iter *cursor)
{
	cursor->is_restarted = false;
	cursor->index++;
	return (iter_at(cursor));
}

struct dma_fence *
dma_resv_iter_first(struct dma_resv_iter *cursor)
{
	return (iter_first(cursor, true));
}

struct dma_fence *
dma_resv_iter_next(struct dma_resv_iter *cursor)
{
	return (dma_resv_iter_next_unlocked(cursor));
}

void
dma_resv_iter_end(struct dma_resv_iter *cursor)
{
	iter_release(cursor);
}

int
dma_resv_get_fences(struct dma_resv *obj, enum dma_resv_usage usage,
    unsigned int *num_fences, struct dma_fence ***fences)
{
	u8 *usages;
	int ret;

	ret = resv_snapshot(obj, usage, false, fences, &usages, num_fences);
	kfree(usages);
	return (ret);
}

int
dma_resv_get_singleton(struct dma_resv *obj, enum dma_resv_usage usage,
    struct dma_fence **fence)
{
	struct dma_fence_array *array;
	struct dma_fence **fences;
	unsigned int count;
	int ret;

	ret = dma_resv_get_fences(obj, usage, &count, &fences);
	if (ret != 0)
		return (ret);
	if (count == 0) {
		*fence = NULL;
		kfree(fences);
		return (0);
	}
	if (count == 1) {
		*fence = fences[0];
		kfree(fences);
		return (0);
	}
	array = dma_fence_array_create(count, fences,
	    dma_fence_context_alloc(1), 1);
	if (array == NULL) {
		while (count > 0)
			dma_fence_put(fences[--count]);
		kfree(fences);
		return (-ENOMEM);
	}
	*fence = &array->base;
	return (0);
}

void
dma_resv_set_deadline(struct dma_resv *obj, enum dma_resv_usage usage,
    ktime_t deadline)
{
	struct dma_resv_iter cursor;
	struct dma_fence *fence;

	dma_resv_iter_begin(&cursor, obj, usage);
	dma_resv_for_each_fence_unlocked(&cursor, fence)
		dma_fence_set_deadline(fence, deadline);
	dma_resv_iter_end(&cursor);
}

void
dma_resv_describe(struct dma_resv *obj, struct seq_file *seq)
{
	static const char *const names[] = {
		"kernel", "write", "read", "bookkeep"
	};
	struct dma_resv_iter cursor;
	struct dma_fence *fence;

	dma_resv_iter_begin(&cursor, obj, DMA_RESV_USAGE_BOOKKEEP);
	dma_resv_for_each_fence_unlocked(&cursor, fence) {
		seq_printf(seq, "\t%s fence:", names[dma_resv_iter_usage(&cursor)]);
		dma_fence_describe(fence, seq);
	}
	dma_resv_iter_end(&cursor);
}

/*
 * Replacing or copying fence lists needs the core's list itself; no i915
 * path uses them. Not supported: they warn and change nothing
 * (replace) or fail (copy).
 */
void
dma_resv_replace_fences(struct dma_resv *obj, uint64_t context,
    struct dma_fence *fence, enum dma_resv_usage usage)
{
	WARN_ONCE(1, "dma_resv_replace_fences is not supported");
}

int
dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src)
{
	WARN_ONCE(1, "dma_resv_copy_fences is not supported");
	return (-ENOTSUPP);
}
