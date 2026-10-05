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
	 * Every wound/wait lock a driver can take is a reservation lock of
	 * the core, whose stamps come from the core's class.
	 */
	WARN_ONCE(ww_class != &reservation_ww_class,
	    "ww_acquire_init: class %s is not supported; using the "
	    "reservation class", ww_class != NULL ? ww_class->acquire_name :
	    "(null)");
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

	if (ctx != NULL) {
		obj->lock.ctx = NULL;
		WARN_ON(ctx->acquired == 0);
		ctx->acquired--;
		drm_illumos_resv_unlock_ctx(obj, ctx->stamp);
	} else {
		(dma_resv_unlock)(obj);
	}
}

bool
lkpi_dma_resv_held(struct dma_resv *obj)
{
	return (drm_illumos_resv_is_locked(obj));
}

/* --- ww_mutex: only the locks of reservation objects ------------------ */

static struct dma_resv *
ww_resv(struct ww_mutex *lock)
{
	return (container_of(lock, struct dma_resv, lock));
}

void
lkpi_ww_mutex_init(struct ww_mutex *lock, struct ww_class *c)
{
	/* A stand-alone ww_mutex has no core lock behind it. */
	WARN(1, "ww_mutex_init: only reservation locks are supported");
	memset(lock, 0, sizeof (*lock));
	lock->ww_class = c;
}

int
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return (resv_lock(ww_resv(lock), ctx, false, false));
}

int
ww_mutex_lock_interruptible(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx)
{
	return (resv_lock(ww_resv(lock), ctx, false, true));
}

void
ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	(void) resv_lock(ww_resv(lock), ctx, ctx != NULL, false);
}

int
ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx)
{
	return (resv_lock(ww_resv(lock), ctx, ctx != NULL, true));
}

int
ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct dma_resv *obj = ww_resv(lock);

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
	lkpi_dma_resv_unlock(ww_resv(lock));
}

bool
ww_mutex_is_locked(struct ww_mutex *lock)
{
	return (drm_illumos_resv_is_locked(ww_resv(lock)));
}

/* --- fence snapshots ------------------------------------------------- */

/*
 * The fences of `obj` with a usage at least as important as `usage`, each
 * with a reference, in a kmalloc'd array, and the usage class each one
 * was found in: the core gives one snapshot per usage level, so the
 * levels are queried from the most important up and every fence is
 * reported at the first level it appears in (matched by context and
 * seqno, since each snapshot gives new structures).
 */
static int
resv_snapshot(struct dma_resv *obj, enum dma_resv_usage usage,
    struct dma_fence ***fencesp, u8 **usagep, unsigned int *countp)
{
	struct dma_fence **all = NULL, **level = NULL;
	u8 *usages = NULL;
	unsigned int n = 0, cap = 0, u;
	int ret = 0;

	*fencesp = NULL;
	*usagep = NULL;
	*countp = 0;
	for (u = DMA_RESV_USAGE_KERNEL; u <= (unsigned int)usage; u++) {
		uint32_t want = 8, got = 0, i, j;

		for (;;) {
			level = kmalloc_array(want, sizeof (*level), GFP_KERNEL);
			if (level == NULL) {
				ret = -ENOMEM;
				goto fail;
			}
			ret = drm_illumos_resv_fences(obj, u, level, want, &got);
			if (ret != 0) {
				kfree(level);
				level = NULL;
				goto fail;
			}
			if (got <= want)
				break;
			for (i = 0; i < want; i++)
				dma_fence_put(level[i]);
			kfree(level);
			want = got;
		}
		for (i = 0; i < got; i++) {
			bool dup = false;

			for (j = 0; j < n && !dup; j++)
				dup = all[j]->context == level[i]->context &&
				    all[j]->seqno == level[i]->seqno;
			if (dup) {
				dma_fence_put(level[i]);
				continue;
			}
			if (n == cap) {
				unsigned int ncap = cap == 0 ? 8 : cap * 2;
				struct dma_fence **na;
				u8 *nu;

				na = krealloc(all, ncap * sizeof (*all),
				    GFP_KERNEL);
				if (na == NULL) {
					ret = -ENOMEM;
					goto fail_level;
				}
				all = na;
				nu = krealloc(usages, ncap, GFP_KERNEL);
				if (nu == NULL) {
					ret = -ENOMEM;
					goto fail_level;
				}
				usages = nu;
				cap = ncap;
			}
			all[n] = level[i];
			usages[n] = (u8)u;
			n++;
			continue;
fail_level:
			for (; i < got; i++)
				dma_fence_put(level[i]);
			kfree(level);
			level = NULL;
			goto fail;
		}
		kfree(level);
		level = NULL;
	}
	*fencesp = all;
	*usagep = usages;
	*countp = n;
	return (0);
fail:
	while (n > 0)
		dma_fence_put(all[--n]);
	kfree(all);
	kfree(usages);
	return (ret);
}

static void
iter_release(struct dma_resv_iter *cursor)
{
	unsigned int i;

	for (i = 0; i < cursor->num_fences; i++)
		dma_fence_put(cursor->fences[i]);
	kfree(cursor->fences);
	kfree(cursor->lkpi_usages);
	cursor->fences = NULL;
	cursor->lkpi_usages = NULL;
	cursor->num_fences = 0;
	cursor->fence = NULL;
}

static struct dma_fence *
iter_at(struct dma_resv_iter *cursor)
{
	if (cursor->index >= cursor->num_fences) {
		cursor->fence = NULL;
		return (NULL);
	}
	cursor->fence = cursor->fences[cursor->index];
	cursor->fence_usage = cursor->lkpi_usages[cursor->index];
	return (cursor->fence);
}

/*
 * A snapshot failure (no memory) is reported as a WARN and an empty walk
 * is not possible to tell from "no fences" in Linux's interface, so the
 * walk then waits for every fence instead: it reports one signalled stub
 * fence after waiting, which makes callers that test or wait on the
 * fences see an idle reservation only once it is idle.
 */
static struct dma_fence *
iter_first(struct dma_resv_iter *cursor)
{
	iter_release(cursor);
	cursor->index = 0;
	cursor->is_restarted = true;
	if (resv_snapshot(cursor->obj, cursor->usage, &cursor->fences,
	    &cursor->lkpi_usages, &cursor->num_fences) != 0) {
		WARN_ONCE(1, "dma_resv iterator: no memory for a snapshot; "
		    "waiting for the fences instead");
		(void) (dma_resv_wait_timeout)(cursor->obj, cursor->usage,
		    false, MAX_SCHEDULE_TIMEOUT);
		return (NULL);
	}
	return (iter_at(cursor));
}

struct dma_fence *
dma_resv_iter_first_unlocked(struct dma_resv_iter *cursor)
{
	return (iter_first(cursor));
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
	return (iter_first(cursor));
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

	ret = resv_snapshot(obj, usage, fences, &usages, num_fences);
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
