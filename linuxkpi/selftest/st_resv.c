/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: reservations, wound/wait transactions and fence
 * containers of the layer (src/lkpi_resv.c, src/lkpi_fence.c) on the DRM
 * core (facade 1.1):
 *
 * - a driver's fence added to a reservation comes back from the locked
 *   and unlocked walks as the driver's own structure, with its usage;
 *   locked walks see signalled fences, unlocked ones do not; a locked walk
 *   left early holds no reference after the unlock (no leak);
 * - two threads lock two reservations in opposite orders with acquire
 *   contexts and Linux's back-off protocol, many times: no deadlock, no
 *   two holders;
 * - dma_resv_get_singleton()'s fence array reports signalled by polling
 *   its children before anyone enabled signalling, and is released.
 */

#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/ww_mutex.h>

#include "st.h"

/* --- a driver's fence ------------------------------------------------ */

struct st_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *
st_fence_name(struct dma_fence *f)
{
	return ("linuxkpi_selftest");
}

static void
st_fence_release(struct dma_fence *f)
{
	kfree(container_of(f, struct st_fence, base));
}

static const struct dma_fence_ops st_fence_ops = {
	.get_driver_name = st_fence_name,
	.get_timeline_name = st_fence_name,
	.release = st_fence_release,
};

static struct dma_fence *
st_fence_new(u64 context, u64 seqno)
{
	struct st_fence *f = kzalloc(sizeof (*f), GFP_KERNEL);

	if (f == NULL)
		return (NULL);
	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &st_fence_ops, &f->lock, context, seqno);
	return (&f->base);
}

/* --- walks ------------------------------------------------------------ */

static bool
st_resv_add(struct dma_resv *r, struct dma_fence *f, enum dma_resv_usage u)
{
	bool ok;

	if (dma_resv_lock(r, NULL) != 0)
		return (false);
	ok = dma_resv_reserve_fences(r, 1) == 0;
	if (ok)
		dma_resv_add_fence(r, f, u);
	dma_resv_unlock(r);
	return (ok);
}

static void
st_walks(void)
{
	struct dma_resv *r = kzalloc(sizeof (*r), GFP_KERNEL);
	struct dma_resv_iter cur;
	struct dma_fence *f1, *f2, *f;
	u64 ctx = dma_fence_context_alloc(2);
	unsigned int n, refs_before;
	bool own, usage_ok;

	if (r == NULL) {
		CHECK("resv walks: allocation", 0);
		return;
	}
	dma_resv_init(r);
	f1 = st_fence_new(ctx, 1);
	f2 = st_fence_new(ctx + 1, 1);
	if (f1 == NULL || f2 == NULL) {
		CHECK("resv walks: fences", 0);
		goto out;
	}
	CHECK("resv add write fence", st_resv_add(r, f1, DMA_RESV_USAGE_WRITE));
	CHECK("resv add read fence", st_resv_add(r, f2, DMA_RESV_USAGE_READ));

	/* Locked walk: the driver's own structures, each with its usage. */
	n = 0;
	own = true;
	usage_ok = true;
	(void) dma_resv_lock(r, NULL);
	dma_resv_for_each_fence(&cur, r, DMA_RESV_USAGE_READ, f) {
		n++;
		own &= (f == f1 || f == f2);
		if (f == f1)
			usage_ok &= dma_resv_iter_usage(&cur) ==
			    DMA_RESV_USAGE_WRITE;
		if (f == f2)
			usage_ok &= dma_resv_iter_usage(&cur) ==
			    DMA_RESV_USAGE_READ;
	}
	dma_resv_unlock(r);
	CHECK("locked walk sees both fences", n == 2);
	CHECK("locked walk returns the driver's own fences", own);
	CHECK("locked walk reports each fence's usage", usage_ok);

	/* Usage filter: WRITE sees only the write fence. */
	n = 0;
	(void) dma_resv_lock(r, NULL);
	dma_resv_for_each_fence(&cur, r, DMA_RESV_USAGE_WRITE, f)
		n += (f == f1) ? 1 : 100;
	dma_resv_unlock(r);
	CHECK("locked walk filters by usage", n == 1);

	/* A locked walk left early: no reference stays after the unlock. */
	refs_before = kref_read(&f1->refcount);
	(void) dma_resv_lock(r, NULL);
	dma_resv_for_each_fence(&cur, r, DMA_RESV_USAGE_READ, f)
		break;
	CHECK("an early-left walk holds its snapshot while locked",
	    kref_read(&f1->refcount) >= refs_before);
	dma_resv_unlock(r);
	CHECK("an early-left walk releases its snapshot at the unlock",
	    kref_read(&f1->refcount) == refs_before);

	/*
	 * Signalled: unlocked walks skip it, locked walks still see it. (The
	 * core's bridge drops its reference when the fence signals.)
	 */
	dma_fence_signal(f1);
	refs_before = kref_read(&f1->refcount);
	n = 0;
	dma_resv_iter_begin(&cur, r, DMA_RESV_USAGE_READ);
	dma_resv_for_each_fence_unlocked(&cur, f)
		n += (f == f1) ? 100 : 1;
	dma_resv_iter_end(&cur);
	CHECK("unlocked walk skips signalled fences", n == 1);
	n = 0;
	(void) dma_resv_lock(r, NULL);
	dma_resv_for_each_fence(&cur, r, DMA_RESV_USAGE_READ, f)
		n += (f == f1) ? 100 : 1;
	dma_resv_unlock(r);
	CHECK("locked walk sees signalled fences", n == 101);
	CHECK("walks leave the fences' counts as they were",
	    kref_read(&f1->refcount) == refs_before);
	dma_fence_signal(f2);
out:
	if (f1 != NULL)
		dma_fence_put(f1);
	if (f2 != NULL)
		dma_fence_put(f2);
	dma_resv_fini(r);
	kfree(r);
}

/* --- transactions under contention ----------------------------------- */

struct st_ww {
	struct work_struct work;
	struct completion done;
	struct dma_resv *a, *b;
	atomic_t *busy_a, *busy_b;
	int rounds;
	int backoffs;
	bool ok;
};

/* Linux's protocol: lock in order; on -EDEADLK drop all, lock slow. */
static int
st_ww_lock_two(struct dma_resv *x, struct dma_resv *y,
    struct ww_acquire_ctx *ctx, int *backoffs)
{
	struct dma_resv *first = x, *second = y, *contended = NULL;
	int ret;

	for (;;) {
		if (contended != NULL) {
			dma_resv_lock_slow(contended, ctx);
			first = contended;
			second = (contended == x) ? y : x;
		}
		if (contended == NULL) {
			ret = dma_resv_lock(first, ctx);
			if (ret != 0)
				return (ret);
		}
		ret = dma_resv_lock(second, ctx);
		if (ret == 0)
			return (0);
		dma_resv_unlock(first);
		if (ret != -EDEADLK)
			return (ret);
		(*backoffs)++;
		contended = second;
	}
}

static void
st_ww_fn(struct work_struct *w)
{
	struct st_ww *s = container_of(w, struct st_ww, work);
	struct ww_acquire_ctx ctx;
	int i;

	for (i = 0; i < s->rounds; i++) {
		ww_acquire_init(&ctx, &reservation_ww_class);
		if (st_ww_lock_two(s->a, s->b, &ctx, &s->backoffs) != 0) {
			s->ok = false;
			ww_acquire_fini(&ctx);
			break;
		}
		ww_acquire_done(&ctx);
		if (atomic_inc_return(s->busy_a) != 1 ||
		    atomic_inc_return(s->busy_b) != 1)
			s->ok = false;
		cond_resched();
		atomic_dec(s->busy_a);
		atomic_dec(s->busy_b);
		if (ctx.acquired != 2)
			s->ok = false;
		dma_resv_unlock(s->a);
		dma_resv_unlock(s->b);
		if (ctx.acquired != 0)
			s->ok = false;
		ww_acquire_fini(&ctx);
	}
	complete(&s->done);
}

static void
st_transactions(void)
{
	struct dma_resv *ra = kzalloc(sizeof (*ra), GFP_KERNEL);
	struct dma_resv *rb = kzalloc(sizeof (*rb), GFP_KERNEL);
	struct workqueue_struct *wq = alloc_workqueue("lkpi_st_ww",
	    WQ_UNBOUND, 2);
	struct st_ww *s = kcalloc(2, sizeof (*s), GFP_KERNEL);
	atomic_t busy_a = ATOMIC_INIT(0), busy_b = ATOMIC_INIT(0);
	bool done0, done1;
	int i;

	if (ra == NULL || rb == NULL || wq == NULL || s == NULL) {
		CHECK("ww: allocation", 0);
		goto out;
	}
	dma_resv_init(ra);
	dma_resv_init(rb);
	for (i = 0; i < 2; i++) {
		/* Opposite orders: the classic AB/BA deadlock otherwise. */
		s[i].a = (i == 0) ? ra : rb;
		s[i].b = (i == 0) ? rb : ra;
		s[i].busy_a = (i == 0) ? &busy_a : &busy_b;
		s[i].busy_b = (i == 0) ? &busy_b : &busy_a;
		s[i].rounds = 2000;
		s[i].ok = true;
		init_completion(&s[i].done);
		INIT_WORK(&s[i].work, st_ww_fn);
		queue_work(wq, &s[i].work);
	}
	done0 = wait_for_completion_timeout(&s[0].done,
	    msecs_to_jiffies(20000)) > 0;
	done1 = wait_for_completion_timeout(&s[1].done,
	    msecs_to_jiffies(20000)) > 0;
	CHECK("ww: opposite-order transactions finish (no deadlock)",
	    done0 && done1);
	if (!done0 || !done1) {
		/* Workers may still use the objects: leak them. */
		return;
	}
	CHECK("ww: never two holders, counts balanced", s[0].ok && s[1].ok);
	pr_info("linuxkpi_selftest: ww back-offs %d + %d\n", s[0].backoffs,
	    s[1].backoffs);
	dma_resv_fini(ra);
	dma_resv_fini(rb);
out:
	if (wq != NULL)
		destroy_workqueue(wq);
	kfree(s);
	kfree(ra);
	kfree(rb);
}

/* --- fence arrays ----------------------------------------------------- */

static void
st_arrays(void)
{
	struct dma_resv *r = kzalloc(sizeof (*r), GFP_KERNEL);
	struct dma_fence *f1, *f2, *single = NULL;
	u64 ctx = dma_fence_context_alloc(2);

	if (r == NULL) {
		CHECK("arrays: allocation", 0);
		return;
	}
	dma_resv_init(r);
	f1 = st_fence_new(ctx, 1);
	f2 = st_fence_new(ctx + 1, 1);
	if (f1 == NULL || f2 == NULL || !st_resv_add(r, f1,
	    DMA_RESV_USAGE_WRITE) || !st_resv_add(r, f2,
	    DMA_RESV_USAGE_WRITE)) {
		CHECK("arrays: fences", 0);
		goto out;
	}
	CHECK("get_singleton of two fences",
	    dma_resv_get_singleton(r, DMA_RESV_USAGE_WRITE, &single) == 0 &&
	    single != NULL && to_dma_fence_array(single) != NULL);
	if (single == NULL)
		goto out;
	CHECK("array unsignalled while a child is",
	    !dma_fence_is_signaled(single));
	dma_fence_signal(f1);
	CHECK("array unsignalled with one child left",
	    !dma_fence_is_signaled(single));
	dma_fence_set_error(f2, -EIO);
	dma_fence_signal(f2);
	/* Nobody enabled signalling: the array polls its children. */
	CHECK("array signalled once all children are (polled)",
	    dma_fence_is_signaled(single));
	CHECK("array takes the children's first error",
	    dma_fence_get_status(single) == -EIO);
	dma_fence_put(single);
out:
	if (f1 != NULL) {
		dma_fence_signal(f1);
		dma_fence_put(f1);
	}
	if (f2 != NULL) {
		dma_fence_signal(f2);
		dma_fence_put(f2);
	}
	dma_resv_fini(r);
	kfree(r);
}

void
st_resv_tests(void)
{
	st_walks();
	st_transactions();
	st_arrays();
}
