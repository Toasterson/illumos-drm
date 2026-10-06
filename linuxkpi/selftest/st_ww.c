/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: the stand-alone wound/wait mutexes of
 * src/lkpi_ww_mutex.c (DRM's mode-setting locks), alone and in
 * transactions with the DRM core's reservation locks:
 *
 * - trylock and is_locked, without and with an acquire context;
 * - a relock by the holding transaction is -EALREADY;
 * - the younger of two transactions gets -EDEADLK for a lock the older
 *   holds, backs off and recovers with ww_mutex_lock_slow(), while the
 *   older one waits; for two stand-alone locks and for a cycle of a
 *   reservation lock and a stand-alone lock, both ways round;
 * - an unlock wakes a blocked waiter;
 * - two transactions take a reservation lock and a stand-alone lock in
 *   opposite orders many times with Linux's back-off protocol: no
 *   deadlock, never two holders.
 *
 * Every wait is bounded and the state the helper kthreads use is static
 * (st_sched.c's helpers, st.h), so a failure yields FAILs, not a hang.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ww_mutex.h>

#include "st.h"

#define	ST_WW_WAIT_MS	3000
#define	ST_WW_BLOCK_MS	50
#define	ST_WW_ROUNDS	2000

static DEFINE_WW_CLASS(st_ww_class);

/* Waits up to ST_WW_WAIT_MS for *v to reach `want`. */
static bool
st_ww_until(volatile int *v, int want)
{
	int i;

	for (i = 0; i < ST_WW_WAIT_MS; i++) {
		if (READ_ONCE(*v) == want)
			return (true);
		msleep(1);
	}
	return (false);
}

static void
st_ww_basics(void)
{
	struct ww_mutex a, b;
	struct ww_acquire_ctx ctx;

	ww_mutex_init(&a, &st_ww_class);
	ww_mutex_init(&b, &st_ww_class);
	CHECK("ww stand-alone: initially unlocked", !ww_mutex_is_locked(&a));
	CHECK("ww stand-alone: trylock without context",
	    ww_mutex_trylock(&a, NULL) == 1 && ww_mutex_is_locked(&a));
	CHECK("ww stand-alone: trylock of a held lock fails",
	    ww_mutex_trylock(&a, NULL) == 0);
	ww_mutex_unlock(&a);
	CHECK("ww stand-alone: unlocked", !ww_mutex_is_locked(&a));

	ww_acquire_init(&ctx, &st_ww_class);
	CHECK("ww stand-alone: lock in a transaction",
	    ww_mutex_lock(&a, &ctx) == 0 && ctx.acquired == 1);
	CHECK("ww stand-alone: relock is -EALREADY",
	    ww_mutex_lock(&a, &ctx) == -EALREADY &&
	    ww_mutex_lock_interruptible(&a, &ctx) == -EALREADY &&
	    ctx.acquired == 1);
	CHECK("ww stand-alone: trylock in a transaction",
	    ww_mutex_trylock(&b, &ctx) == 1 && ctx.acquired == 2 &&
	    ww_mutex_trylock(&a, &ctx) == 0);
	ww_mutex_unlock(&b);
	ww_mutex_unlock(&a);
	CHECK("ww stand-alone: unlocks count the transaction down",
	    ctx.acquired == 0 && !ww_mutex_is_locked(&a) &&
	    !ww_mutex_is_locked(&b));
	ww_acquire_fini(&ctx);
	ww_mutex_destroy(&a);
	ww_mutex_destroy(&b);
}

/* --- the die rule --------------------------------------------------------- */

static struct st_ww_old {
	struct ww_mutex *x, *y;		/* the older one takes x, then y */
	struct ww_acquire_ctx ctx;
	volatile int stage;		/* 1: holds x; 2: holds x and y */
	volatile int release;
	int ret_y;
} st_ww_old;

static void
st_ww_old_call(void *arg)
{
	struct st_ww_old *o = &st_ww_old;

	if (ww_mutex_lock(o->x, &o->ctx) != 0)
		return;
	WRITE_ONCE(o->stage, 1);
	o->ret_y = ww_mutex_lock(o->y, &o->ctx);
	WRITE_ONCE(o->stage, 2);
	(void) st_ww_until(&o->release, 1);
	if (o->ret_y == 0)
		ww_mutex_unlock(o->y);
	ww_mutex_unlock(o->x);
	ww_acquire_fini(&o->ctx);
}

/*
 * The younger transaction holds y; the older one (a helper) takes x and
 * waits for y; the younger one asks for x and must get -EDEADLK, backs
 * off, and takes x with ww_mutex_lock_slow() once the older one is done.
 */
static void
st_ww_die(const char *what, struct ww_mutex *x, struct ww_mutex *y)
{
	struct st_ww_old *o = &st_ww_old;
	struct ww_acquire_ctx young;
	struct st_call c;
	bool waited, died, recovered, done;
	int ret;

	bzero(o, sizeof (*o));
	o->x = x;
	o->y = y;
	/* Stamps are handed out in order: the first is the older. */
	ww_acquire_init(&o->ctx, &st_ww_class);
	ww_acquire_init(&young, &st_ww_class);
	if (ww_mutex_lock(y, &young) != 0 ||
	    !st_call_start(&c, st_ww_old_call, NULL)) {
		CHECK(what, 0);
		return;
	}
	if (!st_ww_until(&o->stage, 1)) {
		CHECK(what, 0);
		WRITE_ONCE(o->release, 1);
		return;
	}
	msleep(ST_WW_BLOCK_MS);
	/* The older transaction waits for y rather than dying. */
	waited = READ_ONCE(o->stage) == 1;
	ret = ww_mutex_lock(x, &young);
	died = ret == -EDEADLK && young.acquired == 1;
	ww_mutex_unlock(y);
	recovered = st_ww_until(&o->stage, 2) && o->ret_y == 0;
	WRITE_ONCE(o->release, 1);
	/* The slow lock waits without the die rule, then the rest. */
	ww_mutex_lock_slow(x, &young);
	recovered &= ww_mutex_lock(y, &young) == 0 && young.acquired == 2;
	done = st_call_finish(&c, ST_WW_WAIT_MS);
	ww_mutex_unlock(y);
	ww_mutex_unlock(x);
	recovered &= young.acquired == 0;
	ww_acquire_fini(&young);
	CHECK(what, waited && died && recovered && done);
}

/* --- an unlock wakes a waiter ------------------------------------------- */

static struct ww_mutex st_ww_wait_lock;
static volatile int st_ww_wait_got;

static void
st_ww_waiter_call(void *arg)
{
	if (ww_mutex_lock(&st_ww_wait_lock, NULL) != 0)
		return;
	WRITE_ONCE(st_ww_wait_got, 1);
	ww_mutex_unlock(&st_ww_wait_lock);
}

static void
st_ww_wakeup(void)
{
	struct st_call c;
	bool blocked, woken;

	ww_mutex_init(&st_ww_wait_lock, &st_ww_class);
	st_ww_wait_got = 0;
	(void) ww_mutex_lock(&st_ww_wait_lock, NULL);
	if (!st_call_start(&c, st_ww_waiter_call, NULL)) {
		CHECK("ww stand-alone: unlock wakes a blocked waiter", 0);
		ww_mutex_unlock(&st_ww_wait_lock);
		return;
	}
	msleep(ST_WW_BLOCK_MS);
	blocked = !completion_done(&c.done) && READ_ONCE(st_ww_wait_got) == 0;
	ww_mutex_unlock(&st_ww_wait_lock);
	woken = st_call_finish(&c, ST_WW_WAIT_MS) &&
	    READ_ONCE(st_ww_wait_got) == 1;
	CHECK("ww stand-alone: unlock wakes a blocked waiter",
	    blocked && woken && !ww_mutex_is_locked(&st_ww_wait_lock));
}

/* --- mixed transactions in opposite orders ---------------------------- */

static struct st_ww_pair {
	struct ww_mutex *first, *second;
	int errors, backoffs;
} st_ww_pairs[2];
static atomic_t st_ww_inside;

/* Linux's protocol: lock in order; on -EDEADLK drop all, lock slow. */
static void
st_ww_pair_call(void *arg)
{
	struct st_ww_pair *p = arg;
	struct ww_acquire_ctx ctx;
	struct ww_mutex *x, *y, *contended;
	int i, ret;

	for (i = 0; i < ST_WW_ROUNDS; i++) {
		ww_acquire_init(&ctx, &st_ww_class);
		x = p->first;
		y = p->second;
		contended = NULL;
		for (;;) {
			if (contended != NULL) {
				ww_mutex_lock_slow(contended, &ctx);
				x = contended;
				y = (contended == p->first) ? p->second :
				    p->first;
			} else if (ww_mutex_lock(x, &ctx) != 0) {
				p->errors++;
				break;
			}
			ret = ww_mutex_lock(y, &ctx);
			if (ret == 0)
				break;
			ww_mutex_unlock(x);
			if (ret != -EDEADLK) {
				p->errors++;
				break;
			}
			p->backoffs++;
			contended = y;
		}
		if (ctx.acquired != 2) {
			p->errors++;
			ww_acquire_fini(&ctx);
			continue;
		}
		ww_acquire_done(&ctx);
		if (atomic_inc_return(&st_ww_inside) != 1)
			p->errors++;
		cond_resched();
		atomic_dec(&st_ww_inside);
		ww_mutex_unlock(p->first);
		ww_mutex_unlock(p->second);
		if (ctx.acquired != 0)
			p->errors++;
		ww_acquire_fini(&ctx);
	}
}

static void
st_ww_mixed_orders(struct dma_resv *r, struct ww_mutex *s)
{
	struct st_call c[2];
	bool done;
	int i;

	bzero(st_ww_pairs, sizeof (st_ww_pairs));
	atomic_set(&st_ww_inside, 0);
	st_ww_pairs[0].first = &r->lock;
	st_ww_pairs[0].second = s;
	st_ww_pairs[1].first = s;
	st_ww_pairs[1].second = &r->lock;
	for (i = 0; i < 2; i++) {
		if (!st_call_start(&c[i], st_ww_pair_call, &st_ww_pairs[i])) {
			CHECK("ww mixed: opposite orders", 0);
			return;
		}
	}
	done = st_call_finish(&c[0], 10 * ST_WW_WAIT_MS);
	done &= st_call_finish(&c[1], 10 * ST_WW_WAIT_MS);
	CHECK("ww mixed: opposite-order transactions finish (no deadlock)",
	    done);
	CHECK("ww mixed: never two holders, counts balanced", done &&
	    st_ww_pairs[0].errors == 0 && st_ww_pairs[1].errors == 0);
	pr_info("linuxkpi_selftest: ww mixed back-offs %d + %d\n",
	    st_ww_pairs[0].backoffs, st_ww_pairs[1].backoffs);
}

/* Static: helpers left behind by a failure must not touch freed memory. */
static struct ww_mutex st_ww_a, st_ww_b, st_ww_s;
static struct dma_resv st_ww_r;

void
st_ww_tests(void)
{
	st_ww_basics();
	st_ww_wakeup();

	ww_mutex_init(&st_ww_a, &st_ww_class);
	ww_mutex_init(&st_ww_b, &st_ww_class);
	ww_mutex_init(&st_ww_s, &st_ww_class);
	dma_resv_init(&st_ww_r);
	st_ww_die("ww stand-alone: younger dies, recovers with lock_slow",
	    &st_ww_a, &st_ww_b);
	st_ww_die("ww mixed: younger dies on a stand-alone lock, recovers",
	    &st_ww_s, &st_ww_r.lock);
	st_ww_die("ww mixed: younger dies on a reservation, recovers",
	    &st_ww_r.lock, &st_ww_s);
	st_ww_mixed_orders(&st_ww_r, &st_ww_s);
	/* Only if nothing is left holding them. */
	if (!ww_mutex_is_locked(&st_ww_r.lock))
		dma_resv_fini(&st_ww_r);
}
