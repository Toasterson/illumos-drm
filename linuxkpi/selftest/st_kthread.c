/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: the kthread workers of src/lkpi_kthread.c (i915's
 * display uses one for its flip and vblank work):
 *
 * - works run once each, in the order they were queued;
 * - kthread_flush_work() of a running work returns only after it ran;
 * - kthread_cancel_work_sync() of a running work that requeues itself
 *   waits for the run, the requeue during the cancel is refused, and the
 *   work neither stays queued nor runs again;
 * - kthread_destroy_worker() of a worker that was created but never
 *   started returns (with and without a work queued on it), and a work
 *   queued before runs.
 *
 * As in st_sched.c, races are forced: a work announces that it runs and
 * holds until the test releases it or sees what it waits for; every wait
 * is bounded, and the state is static, so a failure yields FAILs rather
 * than a hang or a use after free.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "st.h"

#define	ST_KT_WAIT_MS	3000
#define	ST_KT_BLOCK_MS	50
#define	ST_KT_ORDER	16

static struct st_kt {
	struct kthread_worker *worker;
	struct kthread_work w[ST_KT_ORDER];
	struct kthread_work hold_w;
	spinlock_t lock;
	int order[ST_KT_ORDER];
	int n;
	struct completion started;
	volatile int hold;
	volatile int ran;
	atomic_t runs;
	volatile int requeue_refused;
	volatile int requeue_accepted;
} st_kt;

/* --- ordering ------------------------------------------------------------ */

static void
st_kt_order_fn(struct kthread_work *w)
{
	struct st_kt *t = &st_kt;
	int i = (int)(w - t->w);

	spin_lock(&t->lock);
	if (t->n < ST_KT_ORDER)
		t->order[t->n] = i;
	t->n++;
	spin_unlock(&t->lock);
}

static void st_kt_hold_fn(struct kthread_work *w);

static void
st_kt_ordering(void)
{
	struct st_kt *t = &st_kt;
	bool queued = true, again, in_order;
	int i;

	bzero(t, sizeof (*t));
	spin_lock_init(&t->lock);
	init_completion(&t->started);
	atomic_set(&t->runs, 0);
	t->hold = 1;
	t->worker = kthread_run_worker(0, "st_kt_order");
	if (IS_ERR(t->worker)) {
		CHECK("kthread worker: ordering", 0);
		return;
	}
	/* A work that holds the worker while the others are queued. */
	kthread_init_work(&t->hold_w, st_kt_hold_fn);
	(void) kthread_queue_work(t->worker, &t->hold_w);
	if (!st_wait(&t->started, ST_KT_WAIT_MS)) {
		CHECK("kthread worker: ordering", 0);
		t->hold = 0;
		return;
	}
	for (i = 0; i < ST_KT_ORDER; i++) {
		kthread_init_work(&t->w[i], st_kt_order_fn);
		queued &= kthread_queue_work(t->worker, &t->w[i]);
	}
	/* A queued work is not queued twice. */
	again = kthread_queue_work(t->worker, &t->w[ST_KT_ORDER / 2]);
	CHECK("kthread worker: a queued work is queued once", queued && !again);
	t->hold = 0;
	kthread_flush_worker(t->worker);
	in_order = t->n == ST_KT_ORDER;
	for (i = 0; i < ST_KT_ORDER && in_order; i++)
		in_order = t->order[i] == i;
	CHECK("kthread worker: works run once each, in queue order", in_order);
	kthread_destroy_worker(t->worker);
	t->worker = NULL;
}

/* --- flush and cancel of a running work ------------------------------- */

static void
st_kt_hold_fn(struct kthread_work *w)
{
	struct st_kt *t = &st_kt;

	if (atomic_inc_return(&t->runs) == 1)
		complete(&t->started);
	st_hold_sleep(&t->hold);
	WRITE_ONCE(t->ran, 1);
}

static void
st_kt_flush_call(void *arg)
{
	kthread_flush_work(&st_kt.w[0]);
}

static void
st_kt_flush_running(void)
{
	struct st_kt *t = &st_kt;
	struct st_call c;
	bool blocked, done;

	bzero(t, sizeof (*t));
	init_completion(&t->started);
	atomic_set(&t->runs, 0);
	t->hold = 1;
	t->worker = kthread_run_worker(0, "st_kt_flush");
	if (IS_ERR(t->worker)) {
		CHECK("kthread_flush_work of a running work", 0);
		return;
	}
	kthread_init_work(&t->w[0], st_kt_hold_fn);
	(void) kthread_queue_work(t->worker, &t->w[0]);
	if (!st_wait(&t->started, ST_KT_WAIT_MS)) {
		CHECK("kthread_flush_work of a running work", 0);
		t->hold = 0;
		return;
	}
	if (!st_call_start(&c, st_kt_flush_call, NULL)) {
		CHECK("kthread_flush_work of a running work", 0);
		t->hold = 0;
		return;
	}
	msleep(ST_KT_BLOCK_MS);
	blocked = !completion_done(&c.done) && READ_ONCE(t->ran) == 0;
	t->hold = 0;
	done = st_call_finish(&c, ST_KT_WAIT_MS);
	CHECK("kthread_flush_work waits for the running work",
	    blocked && done && READ_ONCE(t->ran) == 1);
	if (done) {
		kthread_destroy_worker(t->worker);
		t->worker = NULL;
	}
}

/*
 * Runs until the cancel is under way (work->canceling, set by
 * kthread_cancel_work_sync() while it waits for the run), then requeues
 * itself: that must be refused.
 */
static void
st_kt_requeue_fn(struct kthread_work *w)
{
	struct st_kt *t = &st_kt;
	int i;

	if (atomic_inc_return(&t->runs) == 1)
		complete(&t->started);
	for (i = 0; i < ST_KT_WAIT_MS && READ_ONCE(w->canceling) == 0; i++)
		msleep(1);
	if (kthread_queue_work(t->worker, w))
		t->requeue_accepted++;
	else
		t->requeue_refused++;
}

static void
st_kt_cancel_call(void *arg)
{
	(void) kthread_cancel_work_sync(&st_kt.w[0]);
}

static void
st_kt_cancel_running(void)
{
	struct st_kt *t = &st_kt;
	struct st_call c;
	bool done, idle;
	int runs;

	bzero(t, sizeof (*t));
	init_completion(&t->started);
	atomic_set(&t->runs, 0);
	t->worker = kthread_run_worker(0, "st_kt_cancel");
	if (IS_ERR(t->worker)) {
		CHECK("kthread_cancel_work_sync of a self-requeueing work", 0);
		return;
	}
	kthread_init_work(&t->w[0], st_kt_requeue_fn);
	(void) kthread_queue_work(t->worker, &t->w[0]);
	if (!st_wait(&t->started, ST_KT_WAIT_MS) ||
	    !st_call_start(&c, st_kt_cancel_call, NULL)) {
		CHECK("kthread_cancel_work_sync of a self-requeueing work", 0);
		return;
	}
	done = st_call_finish(&c, 2 * ST_KT_WAIT_MS);
	runs = atomic_read(&t->runs);
	msleep(ST_KT_BLOCK_MS);
	idle = list_empty(&t->w[0].node) && t->w[0].canceling == 0;
	CHECK("kthread_cancel_work_sync waits for the running work", done);
	CHECK("kthread_cancel_work_sync: the requeue during it is refused",
	    t->requeue_refused == 1 && t->requeue_accepted == 0);
	CHECK("kthread_cancel_work_sync: not queued, not run again",
	    idle && runs == 1 && atomic_read(&t->runs) == 1);
	if (done) {
		kthread_destroy_worker(t->worker);
		t->worker = NULL;
	}
}

/* --- destroy of a worker never started ------------------------------ */

static void
st_kt_destroy_call(void *arg)
{
	kthread_destroy_worker(st_kt.worker);
}

static void
st_kt_destroy_unstarted(bool with_work)
{
	struct st_kt *t = &st_kt;
	struct st_call c;
	const char *what = with_work ?
	    "kthread_destroy_worker of an unstarted worker with a work" :
	    "kthread_destroy_worker of an unstarted worker";
	bool done;

	bzero(t, sizeof (*t));
	init_completion(&t->started);
	atomic_set(&t->runs, 0);
	t->worker = kthread_create_worker(0, "st_kt_idle");
	if (IS_ERR(t->worker)) {
		CHECK(what, 0);
		return;
	}
	if (with_work) {
		/* Queueing starts the worker, as on Linux. */
		kthread_init_work(&t->w[0], st_kt_hold_fn);
		(void) kthread_queue_work(t->worker, &t->w[0]);
	}
	if (!st_call_start(&c, st_kt_destroy_call, NULL)) {
		CHECK(what, 0);
		return;
	}
	done = st_call_finish(&c, ST_KT_WAIT_MS);
	CHECK(what, done && (!with_work || (atomic_read(&t->runs) == 1 &&
	    READ_ONCE(t->ran) == 1)));
	if (done)
		t->worker = NULL;
}

void
st_kthread_tests(void)
{
	st_kt_ordering();
	st_kt_flush_running();
	st_kt_cancel_running();
	st_kt_destroy_unstarted(false);
	st_kt_destroy_unstarted(true);
}
