/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of src/lkpi_ww_mutex.c, the stand-alone wound/wait mutexes
 * (DRM's mode-setting locks), with host threads: trylock and is_locked,
 * -EALREADY on a relock, the die rule (the younger transaction gets
 * -EDEADLK and recovers with the slow lock while the older one waits),
 * an unlock waking a blocked waiter, an interrupted wait, and two
 * transactions taking two locks in opposite orders many times. The
 * acquire contexts get their stamps by hand (ww_acquire_init() takes
 * them from the DRM core). The cycles that mix a reservation lock with a
 * stand-alone one need the core and are in the in-kernel selftest
 * (selftest/st_ww.c).
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ww_mutex.h>
#include "lkpi_ww_mutex.h"
#include "test.h"

static void
t_ctx(struct ww_acquire_ctx *ctx, u64 stamp)
{
	memset(ctx, 0, sizeof (*ctx));
	ctx->stamp = stamp;
	ctx->is_wait_die = 1;
}

/* Waits up to ~2 s for *flag to become `want`. */
static int
t_wait_flag(volatile int *flag, int want)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (__atomic_load_n(flag, __ATOMIC_SEQ_CST) == want)
			return (1);
		drv_usecwait(1000);
	}
	return (0);
}

TEST(ww_trylock_relock)
{
	struct ww_mutex a, b;
	struct ww_acquire_ctx ctx;

	(void) lkpi_ww_init();
	lkpi_ww_mutex_init(&a, NULL);
	lkpi_ww_mutex_init(&b, NULL);
	CHECK(lkpi_ww_standalone(&a));
	CHECK(!lkpi_ww_sa_is_locked(&a));
	/* Without a context. */
	CHECK_EQ(lkpi_ww_sa_trylock(&a, NULL), 1);
	CHECK(lkpi_ww_sa_is_locked(&a));
	CHECK_EQ(lkpi_ww_sa_trylock(&a, NULL), 0);
	lkpi_ww_sa_unlock(&a);
	CHECK(!lkpi_ww_sa_is_locked(&a));
	/* With one: counted, and a relock is -EALREADY, not a deadlock. */
	t_ctx(&ctx, 10);
	CHECK_EQ(lkpi_ww_sa_lock(&a, &ctx, false, false), 0);
	CHECK_EQ(lkpi_ww_sa_lock(&a, &ctx, false, false), -EALREADY);
	CHECK_EQ(lkpi_ww_sa_lock(&a, &ctx, true, true), -EALREADY);
	CHECK_EQ(lkpi_ww_sa_trylock(&a, &ctx), 0);
	CHECK_EQ(lkpi_ww_sa_trylock(&b, &ctx), 1);
	CHECK_EQ(ctx.acquired, 2);
	lkpi_ww_sa_unlock(&b);
	lkpi_ww_sa_unlock(&a);
	CHECK_EQ(ctx.acquired, 0);
	/* An unlock of a free lock is refused with a warning. */
	lkpi_host_cmn_quiet = 1;
	lkpi_ww_sa_unlock(&a);
	lkpi_host_cmn_quiet = 0;
	CHECK(!lkpi_ww_sa_is_locked(&a));
}

/* --- two transactions ---------------------------------------------------- */

struct t_old {
	struct ww_mutex *a, *b;
	struct ww_acquire_ctx ctx;
	volatile int stage;	/* 1: holds a; 2: holds a and b */
	volatile int release;	/* set: unlock both and finish */
	int ret_b;
};

static void *
t_old_main(void *arg)
{
	struct t_old *o = arg;

	(void) lkpi_ww_sa_lock(o->a, &o->ctx, false, false);
	__atomic_store_n(&o->stage, 1, __ATOMIC_SEQ_CST);
	/* The older transaction waits for b instead of dying. */
	o->ret_b = lkpi_ww_sa_lock(o->b, &o->ctx, false, false);
	__atomic_store_n(&o->stage, 2, __ATOMIC_SEQ_CST);
	(void) t_wait_flag(&o->release, 1);
	lkpi_ww_sa_unlock(o->b);
	lkpi_ww_sa_unlock(o->a);
	return (NULL);
}

TEST(ww_younger_dies_and_recovers)
{
	struct ww_mutex a, b;
	struct ww_acquire_ctx young;
	struct t_old o = { 0 };
	pthread_t t;

	(void) lkpi_ww_init();
	lkpi_ww_mutex_init(&a, NULL);
	lkpi_ww_mutex_init(&b, NULL);
	o.a = &a;
	o.b = &b;
	t_ctx(&o.ctx, 100);
	t_ctx(&young, 200);

	/* The younger transaction holds b. */
	CHECK_EQ(lkpi_ww_sa_lock(&b, &young, false, false), 0);
	pthread_create(&t, NULL, t_old_main, &o);
	CHECK(t_wait_flag(&o.stage, 1));
	/* The older one now holds a and waits for b. */
	drv_usecwait(20000);
	CHECK_EQ(o.stage, 1);
	/* The younger one wants a: it must die, not wait. */
	CHECK_EQ(lkpi_ww_sa_lock(&a, &young, false, false), -EDEADLK);
	CHECK(young.contending_lock == &a);
	CHECK_EQ(young.acquired, 1);
	CHECK(lkpi_ww_sa_is_locked(&a));
	/* Back-off: drop everything, then wait for the contended lock. */
	lkpi_ww_sa_unlock(&b);
	CHECK_EQ(young.acquired, 0);
	CHECK(t_wait_flag(&o.stage, 2));
	CHECK_EQ(o.ret_b, 0);
	__atomic_store_n(&o.release, 1, __ATOMIC_SEQ_CST);
	/* The slow lock waits without the die rule and gets a. */
	CHECK_EQ(lkpi_ww_sa_lock(&a, &young, true, false), 0);
	CHECK(young.contending_lock == NULL);
	CHECK_EQ(lkpi_ww_sa_lock(&b, &young, false, false), 0);
	CHECK_EQ(young.acquired, 2);
	pthread_join(t, NULL);
	CHECK_EQ(o.ctx.acquired, 0);
	lkpi_ww_sa_unlock(&a);
	lkpi_ww_sa_unlock(&b);
	CHECK_EQ(young.acquired, 0);
}

/* --- a blocked waiter ---------------------------------------------------- */

struct t_waiter {
	struct ww_mutex *lock;
	struct ww_acquire_ctx ctx;
	int use_ctx, intr, signal;
	volatile int started, done;
	int ret;
};

static void *
t_waiter_main(void *arg)
{
	struct t_waiter *w = arg;

	if (w->signal)
		lkpi_host_cv_signal_pending = 1;
	__atomic_store_n(&w->started, 1, __ATOMIC_SEQ_CST);
	w->ret = lkpi_ww_sa_lock(w->lock, w->use_ctx ? &w->ctx : NULL, false,
	    w->intr);
	__atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
	return (NULL);
}

TEST(ww_unlock_wakes_waiter)
{
	struct ww_mutex a;
	struct ww_acquire_ctx holder;
	struct t_waiter w = { 0 };
	pthread_t t;

	(void) lkpi_ww_init();
	lkpi_ww_mutex_init(&a, NULL);
	t_ctx(&holder, 300);
	CHECK_EQ(lkpi_ww_sa_lock(&a, &holder, false, false), 0);
	/*
	 * A younger transaction that holds nothing waits (the die rule
	 * applies only to one that holds locks).
	 */
	w.lock = &a;
	w.use_ctx = 1;
	t_ctx(&w.ctx, 400);
	pthread_create(&t, NULL, t_waiter_main, &w);
	CHECK(t_wait_flag(&w.started, 1));
	drv_usecwait(50000);
	CHECK_EQ(w.done, 0);
	lkpi_ww_sa_unlock(&a);
	CHECK(t_wait_flag(&w.done, 1));
	pthread_join(t, NULL);
	CHECK_EQ(w.ret, 0);
	CHECK(lkpi_ww_sa_is_locked(&a));
	CHECK_EQ(w.ctx.acquired, 1);
	lkpi_ww_sa_unlock(&a);

	/* An interruptible wait that a signal ends: -EINTR, nothing held. */
	CHECK_EQ(lkpi_ww_sa_trylock(&a, NULL), 1);
	memset(&w, 0, sizeof (w));
	w.lock = &a;
	w.intr = 1;
	w.signal = 1;
	pthread_create(&t, NULL, t_waiter_main, &w);
	CHECK(t_wait_flag(&w.done, 1));
	pthread_join(t, NULL);
	CHECK_EQ(w.ret, -EINTR);
	lkpi_ww_sa_unlock(&a);
	CHECK(!lkpi_ww_sa_is_locked(&a));
}

/* --- opposite orders, many times ---------------------------------------- */

#define	T_WW_ROUNDS	20000

struct t_pair {
	struct ww_mutex *first, *second;
	volatile int *inside;
	u64 *stamps;
	int errors, backoffs;
};

static u64 t_ww_stamp;

/* Linux's protocol: lock in order; on -EDEADLK drop all, lock slow. */
static void *
t_pair_main(void *arg)
{
	struct t_pair *p = arg;
	struct ww_acquire_ctx ctx;
	struct ww_mutex *x, *y, *contended;
	int i, ret;

	for (i = 0; i < T_WW_ROUNDS; i++) {
		t_ctx(&ctx, __atomic_add_fetch(&t_ww_stamp, 1,
		    __ATOMIC_SEQ_CST));
		x = p->first;
		y = p->second;
		contended = NULL;
		for (;;) {
			if (contended != NULL) {
				(void) lkpi_ww_sa_lock(contended, &ctx, true,
				    false);
				x = contended;
				y = (contended == p->first) ? p->second :
				    p->first;
			} else if ((ret = lkpi_ww_sa_lock(x, &ctx, false,
			    false)) != 0) {
				p->errors++;
				break;
			}
			ret = lkpi_ww_sa_lock(y, &ctx, false, false);
			if (ret == 0)
				break;
			lkpi_ww_sa_unlock(x);
			if (ret != -EDEADLK) {
				p->errors++;
				break;
			}
			p->backoffs++;
			contended = y;
		}
		if (ctx.acquired != 2) {
			p->errors++;
			continue;
		}
		if (__atomic_add_fetch(p->inside, 1, __ATOMIC_SEQ_CST) != 1)
			p->errors++;
		__atomic_sub_fetch(p->inside, 1, __ATOMIC_SEQ_CST);
		lkpi_ww_sa_unlock(p->first);
		lkpi_ww_sa_unlock(p->second);
		if (ctx.acquired != 0)
			p->errors++;
	}
	return (NULL);
}

TEST(ww_opposite_orders)
{
	struct ww_mutex a, b;
	volatile int inside = 0;
	struct t_pair p[2];
	pthread_t t[2];
	int i;

	(void) lkpi_ww_init();
	lkpi_ww_mutex_init(&a, NULL);
	lkpi_ww_mutex_init(&b, NULL);
	memset(p, 0, sizeof (p));
	p[0].first = &a;
	p[0].second = &b;
	p[1].first = &b;
	p[1].second = &a;
	for (i = 0; i < 2; i++) {
		p[i].inside = &inside;
		pthread_create(&t[i], NULL, t_pair_main, &p[i]);
	}
	for (i = 0; i < 2; i++)
		pthread_join(t[i], NULL);
	CHECK_EQ(p[0].errors + p[1].errors, 0);
	CHECK(!lkpi_ww_sa_is_locked(&a) && !lkpi_ww_sa_is_locked(&b));
	printf("     ww back-offs %d + %d in %d rounds each\n", p[0].backoffs,
	    p[1].backoffs, T_WW_ROUNDS);
}
