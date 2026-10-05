/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: concurrency tests of the scheduling part (work
 * queues, timers, hrtimers, tasklets, RCU, SRCU, wait queues, seqcounts),
 * of the unload quiesce and of the interrupt and PCI glue.
 *
 * Races are forced, not hoped for: a callback announces that it runs
 * (a completion) and then holds until the test releases it, so the
 * operation under test (cancel, flush, delete, kill, grace period) is
 * started while the callback is known to run, from a helper kthread, and
 * the test checks that it has not returned before the release and has
 * returned after it. Every wait is bounded; a callback holds for at most
 * ST_HOLD_MS, so a broken layer yields FAILs, not a hang. Test state is
 * static: a helper left behind by a failure never touches freed memory.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "st.h"

/* The longest a callback holds, and the longest a test waits. */
#define	ST_HOLD_MS	2000
#define	ST_WAIT_MS	3000
/* How long a blocked operation is observed to stay blocked. */
#define	ST_BLOCK_MS	50

static bool
st_wait(struct completion *c, unsigned int ms)
{
	return (wait_for_completion_timeout(c, msecs_to_jiffies(ms)) != 0);
}

/* Holds (sleeping) while *flag is set, at most ST_HOLD_MS. */
static void
st_hold_sleep(volatile int *flag)
{
	int i;

	for (i = 0; i < ST_HOLD_MS && READ_ONCE(*flag); i++)
		msleep(1);
}

/* Holds (spinning, for callbacks that must not sleep). */
static void
st_hold_spin(volatile int *flag)
{
	hrtime_t end = gethrtime() + (hrtime_t)ST_HOLD_MS * 1000000;

	while (READ_ONCE(*flag) && gethrtime() < end)
		udelay(50);
}

/* --- helper threads ---------------------------------------------------- */

/* Runs fn(arg) on a kthread; done completes when it returned. */
struct st_call {
	void (*fn)(void *);
	void *arg;
	struct completion done;
	struct task_struct *k;
};

static int
st_call_main(void *a)
{
	struct st_call *c = a;

	c->fn(c->arg);
	complete(&c->done);
	return (0);
}

static bool
st_call_start(struct st_call *c, void (*fn)(void *), void *arg)
{
	c->fn = fn;
	c->arg = arg;
	init_completion(&c->done);
	c->k = kthread_run(st_call_main, c, "st_call");
	if (IS_ERR(c->k)) {
		c->k = NULL;
		return (false);
	}
	return (true);
}

/* Waits for the call; reaps the kthread only if it returned. */
static bool
st_call_finish(struct st_call *c, unsigned int ms)
{
	if (c->k == NULL || !st_wait(&c->done, ms))
		return (false);
	(void) kthread_stop(c->k);
	c->k = NULL;
	return (true);
}

/* --- work: cancel and flush against a running, self-requeuing item ---- */

static struct st_rq {
	struct work_struct w;
	struct delayed_work dw;
	struct workqueue_struct *wq;
	struct completion started;
	atomic_t runs;
	volatile int hold;
	volatile int stop;
	bool delayed;
} st_rq;

static void
st_rq_fn(struct work_struct *w)
{
	struct st_rq *r = &st_rq;

	if (atomic_inc_return(&r->runs) == 1)
		complete(&r->started);
	st_hold_sleep(&r->hold);
	if (READ_ONCE(r->stop))
		return;
	/* Requeue itself; refused while a cancel runs. */
	if (r->delayed)
		(void) queue_delayed_work(r->wq, &r->dw, 0);
	else
		(void) queue_work(r->wq, &r->w);
}

static void
st_cancel_work_call(void *arg)
{
	(void) cancel_work_sync(&st_rq.w);
}

static void
st_cancel_dwork_call(void *arg)
{
	(void) cancel_delayed_work_sync(&st_rq.dw);
}

static void
st_flush_work_call(void *arg)
{
	(void) flush_work(&st_rq.w);
}

/*
 * cancel_(delayed_)work_sync() twice at once against the running item,
 * which requeues itself when released: both return only after the run,
 * and the item neither stays queued nor runs again.
 */
static void
st_test_cancel_running(bool delayed)
{
	struct st_rq *r = &st_rq;
	struct st_call c[2];
	bool blocked, done, idle;
	int runs, i;
	const char *what = delayed ? "cancel_delayed_work_sync" :
	    "cancel_work_sync";

	bzero(r, sizeof (*r));
	r->delayed = delayed;
	init_completion(&r->started);
	atomic_set(&r->runs, 0);
	r->hold = 1;
	INIT_WORK(&r->w, st_rq_fn);
	INIT_DELAYED_WORK(&r->dw, st_rq_fn);
	r->wq = alloc_workqueue("st_rq", 0, 4);
	if (r->wq == NULL) {
		CHECK(what, 0);
		return;
	}
	if (delayed)
		(void) queue_delayed_work(r->wq, &r->dw, 1);
	else
		(void) queue_work(r->wq, &r->w);
	if (!st_wait(&r->started, ST_WAIT_MS)) {
		CHECK(what, 0);
		r->hold = 0;
		r->stop = 1;
		return;
	}
	for (i = 0; i < 2; i++)
		(void) st_call_start(&c[i], delayed ? st_cancel_dwork_call :
		    st_cancel_work_call, NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&c[0].done) && !completion_done(&c[1].done);
	r->hold = 0;
	done = st_call_finish(&c[0], ST_WAIT_MS);
	done &= st_call_finish(&c[1], ST_WAIT_MS);
	runs = atomic_read(&r->runs);
	msleep(30);
	if (delayed)
		idle = !delayed_work_pending(&r->dw) &&
		    !work_busy(&r->dw.work);
	else
		idle = !work_pending(&r->w) && !work_busy(&r->w);
	if (delayed)
		CHECK("cancel_delayed_work_sync waits for the running item",
		    blocked && done);
	else
		CHECK("cancel_work_sync waits for the running item",
		    blocked && done);
	CHECK(delayed ? "cancel_delayed_work_sync: self-requeue refused" :
	    "cancel_work_sync: self-requeue refused",
	    idle && atomic_read(&r->runs) == runs && runs == 1);
	r->stop = 1;
	if (done)
		destroy_workqueue(r->wq);
}

/*
 * flush_work() of an item that runs and is queued again: returns only
 * after the queued instance ran too.
 */
static void
st_test_flush_running_pending(void)
{
	struct st_rq *r = &st_rq;
	struct st_call c;
	bool blocked, done, requeued;

	bzero(r, sizeof (*r));
	init_completion(&r->started);
	atomic_set(&r->runs, 0);
	r->hold = 1;
	r->stop = 1;		/* no self-requeue: the test queues it */
	INIT_WORK(&r->w, st_rq_fn);
	r->wq = alloc_workqueue("st_fl", 0, 4);
	if (r->wq == NULL) {
		CHECK("flush_work of a running and pending item", 0);
		return;
	}
	(void) queue_work(r->wq, &r->w);
	if (!st_wait(&r->started, ST_WAIT_MS)) {
		CHECK("flush_work of a running and pending item", 0);
		r->hold = 0;
		return;
	}
	requeued = queue_work(r->wq, &r->w);
	(void) st_call_start(&c, st_flush_work_call, NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&c.done);
	r->hold = 0;
	done = st_call_finish(&c, ST_WAIT_MS);
	CHECK("flush_work of a running and pending item",
	    requeued && blocked && done && atomic_read(&r->runs) == 2 &&
	    !work_busy(&r->w));
	if (done)
		destroy_workqueue(r->wq);
}

/* --- work: forward progress of a multi-worker queue -------------------- */

static struct st_fp {
	struct work_struct x, y;
	struct workqueue_struct *wq;
	struct completion y_done;
	atomic_t xruns;
	volatile int y_ok;
} st_fp;

static void
st_fp_y(struct work_struct *w)
{
	complete(&st_fp.y_done);
}

/*
 * X requeues itself while it runs, so the idle worker finds only X
 * (running elsewhere) pending and waits for a run to end; X then queues
 * Y and waits for it. Y must run on the idle worker.
 */
static void
st_fp_x(struct work_struct *w)
{
	struct st_fp *f = &st_fp;

	if (atomic_inc_return(&f->xruns) != 1)
		return;
	(void) queue_work(f->wq, &f->x);
	msleep(20);
	(void) queue_work(f->wq, &f->y);
	f->y_ok = st_wait(&f->y_done, ST_HOLD_MS);
}

static void
st_test_forward_progress(void)
{
	struct st_fp *f = &st_fp;

	bzero(f, sizeof (*f));
	init_completion(&f->y_done);
	atomic_set(&f->xruns, 0);
	INIT_WORK(&f->x, st_fp_x);
	INIT_WORK(&f->y, st_fp_y);
	f->wq = alloc_workqueue("st_fp", 0, 2);
	if (f->wq == NULL) {
		CHECK("multi-worker queue: forward progress", 0);
		return;
	}
	(void) queue_work(f->wq, &f->x);
	flush_workqueue(f->wq);
	CHECK("multi-worker queue: forward progress",
	    f->y_ok && atomic_read(&f->xruns) == 2);
	destroy_workqueue(f->wq);
}

/* --- work: ordered queues ------------------------------------------------- */

#define	ST_ORD_N	16

static struct st_ord {
	struct work_struct w[ST_ORD_N];
	int order[ST_ORD_N];
	atomic_t next;
	atomic_t running;
	volatile int overlap;
} st_ord;

static void
st_ord_fn(struct work_struct *w)
{
	struct st_ord *o = &st_ord;
	int i = (int)(w - o->w);
	int slot;

	if (atomic_inc_return(&o->running) != 1)
		o->overlap = 1;
	slot = atomic_fetch_inc(&o->next);
	if (slot < ST_ORD_N)
		o->order[slot] = i;
	/* Uneven run times: later items would overtake on a wider queue. */
	if ((i % 3) == 0)
		msleep(2);
	atomic_dec(&o->running);
}

static void
st_test_ordered(void)
{
	struct st_ord *o = &st_ord;
	struct workqueue_struct *wq;
	int i, ok = 1;

	bzero(o, sizeof (*o));
	atomic_set(&o->next, 0);
	atomic_set(&o->running, 0);
	wq = alloc_ordered_workqueue("st_ord", 0);
	if (wq == NULL) {
		CHECK("ordered queue", 0);
		return;
	}
	for (i = 0; i < ST_ORD_N; i++)
		INIT_WORK(&o->w[i], st_ord_fn);
	for (i = 0; i < ST_ORD_N; i++)
		(void) queue_work(wq, &o->w[i]);
	flush_workqueue(wq);
	for (i = 0; i < ST_ORD_N; i++)
		ok &= o->order[i] == i;
	CHECK("ordered queue runs items in order, one at a time",
	    ok && !o->overlap && atomic_read(&o->next) == ST_ORD_N);
	destroy_workqueue(wq);
}

/* --- work: disable, destroy with armed delayed items -------------------- */

static struct st_dis {
	struct work_struct w;
	struct delayed_work dw;
	atomic_t runs;
} st_dis;

static void
st_dis_fn(struct work_struct *w)
{
	atomic_inc(&st_dis.runs);
}

static void
st_test_disable_destroy(void)
{
	struct st_dis *d = &st_dis;
	struct workqueue_struct *wq;
	bool ok;

	bzero(d, sizeof (*d));
	atomic_set(&d->runs, 0);
	INIT_WORK(&d->w, st_dis_fn);
	INIT_DELAYED_WORK(&d->dw, st_dis_fn);
	wq = alloc_workqueue("st_dis", 0, 2);
	if (wq == NULL) {
		CHECK("disable_work", 0);
		return;
	}
	ok = !disable_work_sync(&d->w);
	ok &= !disable_work(&d->w);
	ok &= !queue_work(wq, &d->w);
	ok &= !enable_work(&d->w);	/* still disabled once */
	ok &= !queue_work(wq, &d->w);
	ok &= enable_work(&d->w);
	ok &= queue_work(wq, &d->w);
	flush_work(&d->w);
	CHECK("disable_work/enable_work nest and block queueing",
	    ok && atomic_read(&d->runs) == 1);

	(void) queue_delayed_work(wq, &d->dw, msecs_to_jiffies(10000));
	ok = disable_delayed_work_sync(&d->dw);
	ok &= !delayed_work_pending(&d->dw) && !timer_pending(&d->dw.timer);
	ok &= !queue_delayed_work(wq, &d->dw, 0);
	ok &= !mod_delayed_work(wq, &d->dw, 0);
	ok &= enable_delayed_work(&d->dw);
	CHECK("disable_delayed_work_sync disarms and blocks", ok &&
	    atomic_read(&d->runs) == 1);

	/* Destroying the queue disarms the armed item (no late firing). */
	(void) queue_delayed_work(wq, &d->dw, msecs_to_jiffies(40));
	destroy_workqueue(wq);
	ok = !delayed_work_pending(&d->dw) && !timer_pending(&d->dw.timer);
	msleep(80);
	CHECK("destroy_workqueue disarms armed delayed work",
	    ok && atomic_read(&d->runs) == 1);
}

/* --- timers ---------------------------------------------------------------- */

static struct st_tm {
	struct timer_list t, a, b, c;
	struct completion started;
	atomic_t runs;
	volatile int hold;
	volatile int rearm;
	volatile unsigned long seq;
	unsigned long when_a, when_c;
	unsigned long jiffies_a;
	atomic_t ran_b;
} st_tm;

static void
st_tm_fn(struct timer_list *t)
{
	struct st_tm *s = &st_tm;

	if (atomic_inc_return(&s->runs) == 1)
		complete(&s->started);
	st_hold_spin(&s->hold);
	if (s->rearm)
		(void) mod_timer(t, jiffies + 1);
}

static void
st_tm_delete_call(void *arg)
{
	(void) timer_delete_sync(&st_tm.t);
}

static void
st_tm_abc_fn(struct timer_list *t)
{
	struct st_tm *s = &st_tm;

	if (t == &s->a) {
		s->when_a = ++s->seq;
		s->jiffies_a = jiffies;
	} else if (t == &s->c) {
		s->when_c = ++s->seq;
	} else {
		atomic_inc(&s->ran_b);
	}
}

static void
st_test_timers(void)
{
	struct st_tm *s = &st_tm;
	struct st_call c;
	unsigned long start;
	bool blocked, done;
	int runs;

	/* timer_delete_sync() while the function runs and re-arms. */
	bzero(s, sizeof (*s));
	init_completion(&s->started);
	atomic_set(&s->runs, 0);
	atomic_set(&s->ran_b, 0);
	timer_setup(&s->t, st_tm_fn, 0);
	s->hold = 1;
	s->rearm = 1;
	(void) mod_timer(&s->t, jiffies + 1);
	if (!st_wait(&s->started, ST_WAIT_MS)) {
		CHECK("timer_delete_sync waits for the running function", 0);
		s->hold = 0;
		s->rearm = 0;
		(void) timer_delete_sync(&s->t);
		return;
	}
	(void) st_call_start(&c, st_tm_delete_call, NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&c.done);
	s->hold = 0;
	done = st_call_finish(&c, ST_WAIT_MS);
	runs = atomic_read(&s->runs);
	msleep(30);
	CHECK("timer_delete_sync waits for the running function",
	    blocked && done);
	CHECK("timer_delete_sync disarms the re-armed timer",
	    !timer_pending(&s->t) && atomic_read(&s->runs) == runs);
	s->rearm = 0;

	/* timer_shutdown(): later arming is ignored. */
	(void) mod_timer(&s->t, jiffies + msecs_to_jiffies(20));
	CHECK("timer_shutdown disarms", timer_shutdown(&s->t) == 1);
	runs = atomic_read(&s->runs);
	(void) mod_timer(&s->t, jiffies + 1);
	msleep(40);
	CHECK("timer_shutdown: no re-arm", !timer_pending(&s->t) &&
	    atomic_read(&s->runs) == runs);

	/*
	 * The public expires of an armed timer is the driver's (i915 zeroes
	 * it after timer_delete()): writing it must not disturb the layer's
	 * ordering of pending timers.
	 */
	timer_setup(&s->a, st_tm_abc_fn, 0);
	timer_setup(&s->b, st_tm_abc_fn, 0);
	timer_setup(&s->c, st_tm_abc_fn, 0);
	start = jiffies;
	(void) mod_timer(&s->a, start + msecs_to_jiffies(30));
	(void) mod_timer(&s->b, jiffies + msecs_to_jiffies(40));
	(void) mod_timer(&s->c, jiffies + msecs_to_jiffies(50));
	WRITE_ONCE(s->a.expires, 0);
	WRITE_ONCE(s->c.expires, ~0UL);
	/* An arming after the rewrite searches the tree by its keys. */
	(void) mod_timer(&s->b, jiffies + msecs_to_jiffies(35));
	CHECK("timer_delete of a timer between rewritten ones",
	    timer_delete(&s->b) == 1);
	msleep(120);
	CHECK("timers fire in armed order despite rewritten expires",
	    s->when_a == 1 && s->when_c == 2 && atomic_read(&s->ran_b) == 0 &&
	    time_after_eq(s->jiffies_a, start + msecs_to_jiffies(25)) &&
	    !timer_pending(&s->a) && !timer_pending(&s->c));
	(void) timer_delete_sync(&s->a);
	(void) timer_delete_sync(&s->c);
}

/* --- hrtimers -------------------------------------------------------------- */

static struct st_hr {
	struct hrtimer t;
	struct completion started;
	atomic_t runs;
	volatile int hold;
} st_hr;

static enum hrtimer_restart
st_hr_fn(struct hrtimer *t)
{
	struct st_hr *h = &st_hr;

	if (atomic_inc_return(&h->runs) == 1)
		complete(&h->started);
	st_hold_spin(&h->hold);
	(void) hrtimer_forward_now(t, ms_to_ktime(1));
	return (HRTIMER_RESTART);
}

static void
st_hr_cancel_call(void *arg)
{
	(void) hrtimer_cancel(&st_hr.t);
}

/*
 * hrtimer_cancel() (twice at once) while the function runs and returns
 * HRTIMER_RESTART: the cancels wait for it and leave the timer idle.
 */
static void
st_test_hrtimer(void)
{
	struct st_hr *h = &st_hr;
	struct st_call c[2];
	bool running, trycancel, blocked, done;
	int runs, i;

	bzero(h, sizeof (*h));
	init_completion(&h->started);
	atomic_set(&h->runs, 0);
	hrtimer_setup(&h->t, st_hr_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	h->hold = 1;
	hrtimer_start(&h->t, ms_to_ktime(1), HRTIMER_MODE_REL);
	if (!st_wait(&h->started, ST_WAIT_MS)) {
		CHECK("hrtimer_cancel against HRTIMER_RESTART", 0);
		h->hold = 0;
		(void) hrtimer_cancel(&h->t);
		return;
	}
	running = hrtimer_callback_running(&h->t) &&
	    !hrtimer_is_queued(&h->t) && hrtimer_active(&h->t);
	trycancel = hrtimer_try_to_cancel(&h->t) == -1;
	for (i = 0; i < 2; i++)
		(void) st_call_start(&c[i], st_hr_cancel_call, NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&c[0].done) && !completion_done(&c[1].done);
	h->hold = 0;
	done = st_call_finish(&c[0], ST_WAIT_MS);
	done &= st_call_finish(&c[1], ST_WAIT_MS);
	runs = atomic_read(&h->runs);
	msleep(30);
	CHECK("hrtimer_callback_running/is_queued while running", running);
	CHECK("hrtimer_try_to_cancel of a running timer is -1", trycancel);
	CHECK("hrtimer_cancel waits for a restarting function",
	    blocked && done);
	CHECK("hrtimer_cancel leaves the restarting timer idle",
	    !hrtimer_active(&h->t) && atomic_read(&h->runs) == runs);

	/* Repeated start/cancel against free-running restarts. */
	for (i = 0; i < 20; i++) {
		hrtimer_start(&h->t, ms_to_ktime(1), HRTIMER_MODE_REL);
		msleep(i % 3);
		(void) hrtimer_cancel(&h->t);
		if (hrtimer_active(&h->t))
			break;
	}
	runs = atomic_read(&h->runs);
	msleep(20);
	CHECK("hrtimer start/cancel cycles end idle", i == 20 &&
	    atomic_read(&h->runs) == runs);
}

/* schedule_hrtimeout(): relative, absolute, past. */
static void
st_test_schedule_hrtimeout(void)
{
	ktime_t t0, kt;
	int r1, r2, r3;
	s64 e1, e2;

	t0 = ktime_get();
	kt = ms_to_ktime(20);
	set_current_state(TASK_UNINTERRUPTIBLE);
	r1 = schedule_hrtimeout(&kt, HRTIMER_MODE_REL);
	e1 = ktime_get() - t0;
	t0 = ktime_get();
	kt = ktime_add(t0, ms_to_ktime(20));
	set_current_state(TASK_UNINTERRUPTIBLE);
	r2 = schedule_hrtimeout(&kt, HRTIMER_MODE_ABS);
	e2 = ktime_get() - t0;
	kt = ktime_sub(ktime_get(), ms_to_ktime(5));
	set_current_state(TASK_UNINTERRUPTIBLE);
	r3 = schedule_hrtimeout(&kt, HRTIMER_MODE_ABS);
	CHECK("schedule_hrtimeout relative", r1 == 0 && e1 >= 20000000LL &&
	    e1 < 1000000000LL);
	CHECK("schedule_hrtimeout absolute", r2 == 0 && e2 >= 20000000LL &&
	    e2 < 1000000000LL);
	CHECK("schedule_hrtimeout in the past", r3 == 0 &&
	    task_is_running(current));
}

/* --- tasklets --------------------------------------------------------------- */

static struct st_tl {
	struct tasklet_struct t;
	struct completion started;
	atomic_t runs;
	volatile int hold;
} st_tl;

static void
st_tl_fn(struct tasklet_struct *t)
{
	struct st_tl *s = &st_tl;

	atomic_inc(&s->runs);
	complete(&s->started);
	st_hold_spin(&s->hold);
}

static void
st_tl_kill_call(void *arg)
{
	tasklet_kill(&st_tl.t);
}

static void
st_tl_disable_call(void *arg)
{
	tasklet_disable(&st_tl.t);
}

static void
st_test_tasklets(void)
{
	struct st_tl *s = &st_tl;
	struct st_call c[2];
	bool blocked, done, ran;

	bzero(s, sizeof (*s));
	atomic_set(&s->runs, 0);
	init_completion(&s->started);
	tasklet_setup(&s->t, st_tl_fn);

	/* A disabled tasklet runs only after tasklet_enable(). */
	tasklet_disable(&s->t);
	tasklet_schedule(&s->t);
	msleep(30);
	ran = atomic_read(&s->runs) != 0;
	tasklet_enable(&s->t);
	CHECK("tasklet scheduled while disabled runs after enable",
	    !ran && st_wait(&s->started, ST_WAIT_MS));
	tasklet_kill(&s->t);

	/* tasklet_disable() waits for the running callback. */
	reinit_completion(&s->started);
	s->hold = 1;
	tasklet_schedule(&s->t);
	if (!st_wait(&s->started, ST_WAIT_MS)) {
		CHECK("tasklet_disable waits for the running tasklet", 0);
		s->hold = 0;
		return;
	}
	(void) st_call_start(&c[0], st_tl_disable_call, NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&c[0].done);
	s->hold = 0;
	done = st_call_finish(&c[0], ST_WAIT_MS);
	CHECK("tasklet_disable waits for the running tasklet",
	    blocked && done);
	tasklet_enable(&s->t);

	/* Two tasklet_kill() at once while it runs and is rescheduled. */
	reinit_completion(&s->started);
	atomic_set(&s->runs, 0);
	s->hold = 1;
	tasklet_schedule(&s->t);
	if (!st_wait(&s->started, ST_WAIT_MS)) {
		CHECK("tasklet_kill against a running tasklet", 0);
		s->hold = 0;
		return;
	}
	tasklet_schedule(&s->t);	/* scheduled again while it runs */
	(void) st_call_start(&c[0], st_tl_kill_call, NULL);
	(void) st_call_start(&c[1], st_tl_kill_call, NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&c[0].done) && !completion_done(&c[1].done);
	s->hold = 0;
	done = st_call_finish(&c[0], ST_WAIT_MS);
	done &= st_call_finish(&c[1], ST_WAIT_MS);
	CHECK("tasklet_kill waits for the running and rescheduled tasklet",
	    blocked && done && atomic_read(&s->runs) == 2 &&
	    !test_bit(TASKLET_STATE_SCHED, &s->t.state) &&
	    !test_bit(TASKLET_STATE_RUN, &s->t.state));
}

/* --- RCU and SRCU ------------------------------------------------------------- */

static struct st_rcu {
	struct completion in_reader;
	volatile int hold;
	volatile int stop;
	struct srcu_struct srcu;
	atomic_t loops;
} st_rcu;

/* A reader that stays in its (nested) read section until released. */
static void
st_rcu_holder(void *arg)
{
	rcu_read_lock();
	rcu_read_lock();
	complete(&st_rcu.in_reader);
	st_hold_spin(&st_rcu.hold);
	rcu_read_unlock();
	rcu_read_unlock();
}

static void
st_srcu_holder(void *arg)
{
	int idx = srcu_read_lock(&st_rcu.srcu);

	complete(&st_rcu.in_reader);
	st_hold_sleep(&st_rcu.hold);
	srcu_read_unlock(&st_rcu.srcu, idx);
}

static void
st_sync_rcu_call(void *arg)
{
	synchronize_rcu();
}

static void
st_sync_srcu_call(void *arg)
{
	synchronize_srcu(&st_rcu.srcu);
}

/*
 * Overlapping readers: two threads that are inside a read section nearly
 * all the time, so that one of them is inside at almost every instant.
 * They sleep inside the section (msleep): busy readers of the SYS class
 * on both CPUs of the test guest would starve the test thread. Sleeping
 * in an RCU read section is not allowed to drivers, but the layer's
 * counting tolerates it, and it keeps a reader count above zero as busy
 * readers on more CPUs would. A deadline bounds them.
 */
static void
st_rcu_churn(void *arg)
{
	hrtime_t end = gethrtime() + (hrtime_t)ST_HOLD_MS * 1000000;

	while (!READ_ONCE(st_rcu.stop) && gethrtime() < end) {
		rcu_read_lock();
		msleep(2);
		rcu_read_lock();
		udelay(20);
		rcu_read_unlock();
		rcu_read_unlock();
		atomic_inc(&st_rcu.loops);
	}
}

static void
st_srcu_churn(void *arg)
{
	hrtime_t end = gethrtime() + (hrtime_t)ST_HOLD_MS * 1000000;
	int idx;

	while (!READ_ONCE(st_rcu.stop) && gethrtime() < end) {
		idx = srcu_read_lock(&st_rcu.srcu);
		msleep(2);
		srcu_read_unlock(&st_rcu.srcu, idx);
		atomic_inc(&st_rcu.loops);
	}
}

/*
 * A grace period waits for a reader that was inside when it started, and
 * ends while new readers keep overlapping (the old single count never
 * reached zero then).
 */
static void
st_test_grace_periods(bool srcu)
{
	struct st_rcu *r = &st_rcu;
	struct st_call h, s, ch[2];
	bool blocked, done, all;
	hrtime_t t0, t;
	int i, n;

	if (srcu)
		(void) init_srcu_struct(&r->srcu);
	init_completion(&r->in_reader);
	r->hold = 1;
	r->stop = 0;
	if (!st_call_start(&h, srcu ? st_srcu_holder : st_rcu_holder, NULL) ||
	    !st_wait(&r->in_reader, ST_WAIT_MS)) {
		CHECK(srcu ? "synchronize_srcu" : "synchronize_rcu", 0);
		r->hold = 0;
		return;
	}
	(void) st_call_start(&s, srcu ? st_sync_srcu_call : st_sync_rcu_call,
	    NULL);
	msleep(ST_BLOCK_MS);
	blocked = !completion_done(&s.done);
	r->hold = 0;
	done = st_call_finish(&s, ST_WAIT_MS);
	done &= st_call_finish(&h, ST_WAIT_MS);
	CHECK(srcu ? "synchronize_srcu waits for an earlier reader" :
	    "synchronize_rcu waits for an earlier nested reader",
	    blocked && done);

	atomic_set(&r->loops, 0);
	for (i = 0; i < 2; i++)
		(void) st_call_start(&ch[i], srcu ? st_srcu_churn :
		    st_rcu_churn, NULL);
	msleep(20);
	t0 = gethrtime();
	for (n = 0; n < 10; n++) {
		t = gethrtime();
		if (srcu)
			synchronize_srcu(&r->srcu);
		else
			synchronize_rcu();
		if (gethrtime() - t > 1000000000LL)
			break;
	}
	t = gethrtime() - t0;
	r->stop = 1;
	all = st_call_finish(&ch[0], ST_WAIT_MS);
	all &= st_call_finish(&ch[1], ST_WAIT_MS);
	CHECK(srcu ? "synchronize_srcu ends under overlapping readers" :
	    "synchronize_rcu ends under overlapping readers",
	    n == 10 && t < 5000000000LL && all && atomic_read(&r->loops) > 0);
	if (srcu && all)
		cleanup_srcu_struct(&r->srcu);
}

/* --- wait_event_lock_irq, seqcounts, signals -------------------------------- */

static struct st_wl {
	spinlock_t lock;
	wait_queue_head_t wq;
	int cond;
	volatile int done;
	seqcount_t seq;
	volatile int hold;
	struct completion in_write;
} st_wl;

static void
st_wl_setter(void *arg)
{
	struct st_wl *w = &st_wl;
	int i;

	msleep(20);
	spin_lock_irq(&w->lock);
	w->cond = 1;
	spin_unlock_irq(&w->lock);
	/* Wake until the waiter is out, so a lost wake-up cannot hang. */
	for (i = 0; i < ST_HOLD_MS / 10 && !READ_ONCE(w->done); i++) {
		wake_up_all(&w->wq);
		msleep(10);
	}
}

static void
st_seq_writer(void *arg)
{
	struct st_wl *w = &st_wl;

	write_seqcount_begin(&w->seq);
	complete(&w->in_write);
	st_hold_sleep(&w->hold);
	write_seqcount_end(&w->seq);
}

static void
st_seq_release(void *arg)
{
	msleep(30);
	st_wl.hold = 0;
}

static void
st_test_waits(void)
{
	struct st_wl *w = &st_wl;
	struct st_call c, rel;
	hrtime_t t0, t;
	unsigned int s;
	bool ok;

	bzero(w, sizeof (*w));
	spin_lock_init(&w->lock);
	init_waitqueue_head(&w->wq);
	t0 = gethrtime();
	(void) st_call_start(&c, st_wl_setter, NULL);
	spin_lock_irq(&w->lock);
	wait_event_lock_irq(w->wq, w->cond != 0, w->lock);
	ok = spin_is_locked(&w->lock) && w->cond == 1;
	spin_unlock_irq(&w->lock);
	t = gethrtime() - t0;
	w->done = 1;
	ok &= st_call_finish(&c, ST_WAIT_MS);
	CHECK("wait_event_lock_irq: woken, lock held on return",
	    ok && t < 1000000000LL);

	/* A reader that begins during a write waits for its end. */
	seqcount_init(&w->seq);
	init_completion(&w->in_write);
	w->hold = 1;
	(void) st_call_start(&c, st_seq_writer, NULL);
	if (!st_wait(&w->in_write, ST_WAIT_MS)) {
		CHECK("read_seqcount_begin waits for a writer", 0);
		w->hold = 0;
		return;
	}
	t0 = gethrtime();
	/* Another thread ends the write 30 ms from now. */
	if (!st_call_start(&rel, st_seq_release, NULL)) {
		CHECK("read_seqcount_begin waits for a writer", 0);
		w->hold = 0;
		return;
	}
	s = read_seqcount_begin(&w->seq);
	t = gethrtime() - t0;
	ok = (s & 1) == 0 && !read_seqcount_retry(&w->seq, s);
	ok &= st_call_finish(&c, ST_WAIT_MS);
	ok &= st_call_finish(&rel, ST_WAIT_MS);
	CHECK("read_seqcount_begin waits for a writer",
	    ok && t >= 20000000LL);

	CHECK("signal_pending is false without a signal",
	    !signal_pending(current));
}

/* --- unload quiesce ------------------------------------------------------------ */

static struct st_q {
	struct delayed_work dw;
	struct work_struct blocker, queued;
	struct timer_list t;
	struct task_struct *k;
	volatile int hold;
	volatile int kexit;
	struct completion started;
	atomic_t runs;
} st_q;

static void
st_q_work(struct work_struct *w)
{
	atomic_inc(&st_q.runs);
}

static void
st_q_blocker(struct work_struct *w)
{
	complete(&st_q.started);
	st_hold_sleep(&st_q.hold);
}

static void
st_q_timer(struct timer_list *t)
{
	atomic_inc(&st_q.runs);
}

static int
st_q_kthread(void *arg)
{
	while (!kthread_should_stop())
		msleep(1);
	st_q.kexit = 1;
	return (0);
}

/*
 * lkpi_module_quiesce_range() over single test functions (so that
 * nothing else of this module is touched): an armed delayed item, an
 * item queued behind a blocker, a timer and a kthread are removed.
 */
static void
st_test_quiesce(void)
{
	struct st_q *q = &st_q;
	struct workqueue_struct *wq;
	bool ok;

	bzero(q, sizeof (*q));
	init_completion(&q->started);
	atomic_set(&q->runs, 0);
	INIT_DELAYED_WORK(&q->dw, st_q_work);
	INIT_WORK(&q->blocker, st_q_blocker);
	INIT_WORK(&q->queued, st_q_work);
	timer_setup(&q->t, st_q_timer, 0);
	wq = alloc_ordered_workqueue("st_q", 0);
	if (wq == NULL) {
		CHECK("lkpi_module_quiesce_range", 0);
		return;
	}
	q->hold = 1;
	(void) queue_work(wq, &q->blocker);
	(void) st_wait(&q->started, ST_WAIT_MS);
	(void) queue_work(wq, &q->queued);
	(void) queue_delayed_work(wq, &q->dw, msecs_to_jiffies(10000));
	(void) mod_timer(&q->t, jiffies + msecs_to_jiffies(10000));
	q->k = kthread_run(st_q_kthread, NULL, "st_q");

	lkpi_module_quiesce_range((const void *)st_q_work, 1);
	lkpi_module_quiesce_range((const void *)st_q_timer, 1);
	if (!IS_ERR(q->k))
		lkpi_module_quiesce_range((const void *)st_q_kthread, 1);
	ok = !work_pending(&q->queued) && !delayed_work_pending(&q->dw) &&
	    !timer_pending(&q->t) && (IS_ERR(q->k) || q->kexit);
	q->hold = 0;
	flush_workqueue(wq);
	CHECK("lkpi_module_quiesce_range removes work, timers, kthreads",
	    ok && atomic_read(&q->runs) == 0);
	destroy_workqueue(wq);
}

/* --- interrupts and PCI ---------------------------------------------------- */

static int st_irq_cookie;

static irqreturn_t
st_irq_handler(int irq, void *dev_id)
{
	return (IRQ_NONE);
}

static irqreturn_t
st_irq_thread(int irq, void *dev_id)
{
	return (IRQ_HANDLED);
}

/*
 * No interrupt can be raised from here (pci-testdev has none to trigger,
 * and soft interrupts are not part of the DDI the test links against),
 * so these check the refusal paths, and request/free/synchronize on a
 * vector if the device has one.
 */
static void
st_test_irq(struct pci_dev *pdev)
{
	const void *name;
	int irq, n;
	bool ok;

	ok = request_irq(0, st_irq_handler, 0, "st", &st_irq_cookie) ==
	    -EINVAL;
	ok &= request_irq(100000, st_irq_handler, 0, "st", &st_irq_cookie) ==
	    -EINVAL;
	synchronize_irq(0);
	disable_irq(0);
	enable_irq(0);
	CHECK("request_irq refuses unallocated vectors", ok);
	if (pdev == NULL)
		return;

	n = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (n <= 0) {
		pr_info("linuxkpi_selftest: device has no interrupt vector "
		    "(%d), vector tests skipped\n", n);
		return;
	}
	irq = pci_irq_vector(pdev, 0);
	ok = request_threaded_irq(irq, NULL, NULL, 0, "st", &st_irq_cookie) ==
	    -EINVAL;
	ok &= request_irq(irq, st_irq_handler, IRQF_SHARED, "st",
	    &st_irq_cookie) == 0;
	ok &= request_irq(irq, st_irq_handler, IRQF_SHARED, "st2",
	    &st_irq_cookie) == -EBUSY;
	ok &= free_irq(irq, &n) == NULL;
	disable_irq(irq);
	disable_irq_nosync(irq);
	enable_irq(irq);
	enable_irq(irq);
	synchronize_irq(irq);
	name = free_irq(irq, &st_irq_cookie);
	ok &= name != NULL && strcmp(name, "st") == 0;
	ok &= request_threaded_irq(irq, st_irq_handler, st_irq_thread,
	    IRQF_SHARED, "st_thr", &st_irq_cookie) == 0;
	synchronize_irq(irq);
	ok &= free_irq(irq, &st_irq_cookie) != NULL;
	pci_free_irq_vectors(pdev);
	CHECK("request/free/synchronize/disable/enable of a vector", ok);
}

static void
st_test_pci(struct pci_dev *pdev)
{
	struct pci_dev *hb, *hb2;
	uint32_t refs;
	u32 v32;
	u16 v16;
	bool ok;
	int ret;

	CHECK("pci_get_device finds the attached device",
	    pdev != NULL && !pdev->lkpi_foreign);
	if (pdev == NULL)
		return;
	CHECK("config space size", (pdev->cfg_size == 256 ||
	    pdev->cfg_size == 4096) &&
	    (pdev->pcie_cap != 0 || pdev->cfg_size == 256));
	ok = pci_read_config_dword(pdev, pdev->cfg_size, &v32) == -EINVAL &&
	    v32 == ~0U;
	ok &= pci_read_config_dword(pdev, pdev->cfg_size - 4, &v32) == 0;
	CHECK("config reads end at the config space size", ok);

	/* The host bridge, found while this device attaches. */
	hb = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	CHECK("host bridge found during attach", hb != NULL);
	if (hb == NULL)
		return;
	hb2 = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	refs = hb->dev.lkpi_refs;
	(void) pci_dev_get(hb);
	pci_dev_put(hb);
	CHECK("host bridge wrapper is cached and reference counted",
	    hb2 == hb && hb->dev.lkpi_refs == refs);
	pci_dev_put(hb2);
	if (hb->lkpi_foreign) {
		ret = pci_write_config_dword(hb, PCI_VENDOR_ID, 0);
		CHECK("host bridge: write outside MCHBAR/DEVEN refused",
		    ret == -EPERM);
		ret = pci_write_config_word(hb, 0x100, 0);
		CHECK("host bridge: write beyond the config space refused",
		    ret != 0);
	}
	ok = pci_bus_read_config_word(hb->bus, PCI_DEVFN(0, 0),
	    PCI_VENDOR_ID, &v16) == 0 && v16 == hb->vendor;
	CHECK("pci_bus_read_config_word reaches the host bridge", ok);
	pci_dev_put(hb);
}

void
st_sched_tests(void)
{
	struct pci_dev *pdev;

	st_test_cancel_running(false);
	st_test_cancel_running(true);
	st_test_flush_running_pending();
	st_test_forward_progress();
	st_test_ordered();
	st_test_disable_destroy();
	st_test_timers();
	st_test_hrtimer();
	st_test_schedule_hrtimeout();
	st_test_tasklets();
	st_test_grace_periods(false);
	st_test_grace_periods(true);
	st_test_waits();
	st_test_quiesce();

	pdev = pci_get_device(0x1b36, 0x0005, NULL);
	st_test_irq(pdev);
	st_test_pci(pdev);
	pci_dev_put(pdev);
}
