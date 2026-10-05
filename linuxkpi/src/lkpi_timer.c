/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's timers and high-resolution timers (<linux/timer.h>,
 * <linux/hrtimer.h>).
 *
 * Every pending timer is in one AVL tree (avl_create(9F)-style, embedded
 * node, no allocation) ordered by expiry in jiffies and then by an arming
 * sequence number, which is non-zero exactly while the timer is pending.
 * One layer thread (a single-thread ddi_taskq(9F) at high priority)
 * sleeps until the earliest expiry with cv_reltimedwait(9F), takes due
 * timers out of the tree one at a time and calls their functions with the
 * timer lock dropped. Arming a timer that becomes the earliest wakes the
 * thread.
 *
 * The thread records the timer whose function runs (lkpi_tm_running) and
 * never dereferences the timer after the function returns; it only
 * compares the pointer, so the function may free its timer. Waiting for a
 * running function (timer_delete_sync()) uses that record; it returns at
 * once on the timer thread itself, where the function cannot be running
 * concurrently.
 *
 * hrtimers are timer_lists whose function re-arms them for
 * HRTIMER_RESTART; their nanosecond expiry is rounded up to a jiffy and
 * then to illumos' clock tick.
 */

#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/* lkpi_state: timer_shutdown_sync() was called; arming is ignored. */
#define	LKPI_TIMER_SHUTDOWN	0x1

static kmutex_t lkpi_tm_lock;
static kcondvar_t lkpi_tm_cv;		/* the thread: earliest timer changed */
static kcondvar_t lkpi_tm_done_cv;	/* a timer function returned */
static avl_tree_t lkpi_tm_tree;
static uint64_t lkpi_tm_seq;
static struct timer_list *lkpi_tm_running;
static kthread_t *lkpi_tm_thread;
static boolean_t lkpi_tm_exiting;
static ddi_taskq_t *lkpi_tm_tq;

static int
lkpi_tm_compare(const void *a, const void *b)
{
	const struct timer_list *ta = a, *tb = b;

	if (ta->expires != tb->expires)
		return (ta->expires < tb->expires ? -1 : 1);
	if (ta->lkpi_seq != tb->lkpi_seq)
		return (ta->lkpi_seq < tb->lkpi_seq ? -1 : 1);
	return (0);
}

static inline boolean_t
lkpi_tm_pending(const struct timer_list *t)
{
	return (t->lkpi_seq != 0);
}

static void
lkpi_tm_remove_locked(struct timer_list *t)
{
	avl_remove(&lkpi_tm_tree, t);
	t->lkpi_seq = 0;
}

/*
 * (Re-)arms t for `expires` with the timer lock held; returns whether it
 * was pending. A shut-down timer, or any timer while the layer is not
 * running, stays idle.
 */
static int
lkpi_tm_arm_locked(struct timer_list *t, unsigned long expires)
{
	int was = 0;

	if (lkpi_tm_pending(t)) {
		lkpi_tm_remove_locked(t);
		was = 1;
	}
	if ((t->lkpi_state & LKPI_TIMER_SHUTDOWN) != 0)
		return (was);
	if (lkpi_tm_tq == NULL || lkpi_tm_exiting) {
		WARN_ONCE(1, "linuxkpi: timer armed while the layer is not "
		    "running");
		return (was);
	}
	t->expires = expires;
	t->lkpi_seq = ++lkpi_tm_seq;
	avl_add(&lkpi_tm_tree, t);
	if (avl_first(&lkpi_tm_tree) == t)
		cv_signal(&lkpi_tm_cv);
	return (was);
}

void
lkpi_timer_setup(struct timer_list *t, void (*fn)(struct timer_list *),
    unsigned int flags)
{
	bzero(t, sizeof (*t));
	t->function = fn;
	t->flags = flags;
}

int
mod_timer(struct timer_list *t, unsigned long expires)
{
	int was;

	mutex_enter(&lkpi_tm_lock);
	if (lkpi_tm_pending(t) && t->expires == expires)
		was = 1;
	else
		was = lkpi_tm_arm_locked(t, expires);
	mutex_exit(&lkpi_tm_lock);
	return (was);
}

int
mod_timer_pending(struct timer_list *t, unsigned long expires)
{
	int was = 0;

	mutex_enter(&lkpi_tm_lock);
	if (lkpi_tm_pending(t))
		was = lkpi_tm_arm_locked(t, expires);
	mutex_exit(&lkpi_tm_lock);
	return (was);
}

/* Arms t, or moves a pending t to an earlier expiry (never later). */
int
timer_reduce(struct timer_list *t, unsigned long expires)
{
	int was;

	mutex_enter(&lkpi_tm_lock);
	if (lkpi_tm_pending(t) && !time_before(expires, t->expires))
		was = 1;
	else
		was = lkpi_tm_arm_locked(t, expires);
	mutex_exit(&lkpi_tm_lock);
	return (was);
}

void
add_timer(struct timer_list *t)
{
	mutex_enter(&lkpi_tm_lock);
	WARN_ON(lkpi_tm_pending(t));
	(void) lkpi_tm_arm_locked(t, t->expires);
	mutex_exit(&lkpi_tm_lock);
}

int
timer_delete(struct timer_list *t)
{
	int was = 0;

	mutex_enter(&lkpi_tm_lock);
	if (lkpi_tm_pending(t)) {
		lkpi_tm_remove_locked(t);
		was = 1;
	}
	mutex_exit(&lkpi_tm_lock);
	return (was);
}

int
timer_pending(const struct timer_list *t)
{
	return (__atomic_load_n(&t->lkpi_seq, __ATOMIC_RELAXED) != 0);
}

/* Whether the caller may wait for a timer function. */
static boolean_t
lkpi_tm_may_wait(void)
{
	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: waiting for a timer in an interrupt");
		return (B_FALSE);
	}
	return (curthread != lkpi_tm_thread);
}

/*
 * Disarms t and waits until its function is not running. A function that
 * re-arms its timer is disarmed again after it returns, as on Linux.
 */
int
timer_delete_sync(struct timer_list *t)
{
	boolean_t wait = lkpi_tm_may_wait();
	int was = 0;

	mutex_enter(&lkpi_tm_lock);
	for (;;) {
		if (lkpi_tm_pending(t)) {
			lkpi_tm_remove_locked(t);
			was = 1;
		}
		if (!wait || lkpi_tm_running != t)
			break;
		cv_wait(&lkpi_tm_done_cv, &lkpi_tm_lock);
	}
	mutex_exit(&lkpi_tm_lock);
	return (was);
}

int
timer_shutdown_sync(struct timer_list *t)
{
	mutex_enter(&lkpi_tm_lock);
	t->lkpi_state |= LKPI_TIMER_SHUTDOWN;
	mutex_exit(&lkpi_tm_lock);
	return (timer_delete_sync(t));
}

void
lkpi_timer_wait_idle(struct timer_list *t)
{
	if (!lkpi_tm_may_wait())
		return;
	mutex_enter(&lkpi_tm_lock);
	while (lkpi_tm_running == t)
		cv_wait(&lkpi_tm_done_cv, &lkpi_tm_lock);
	mutex_exit(&lkpi_tm_lock);
}

/* The timer thread. */
static void
lkpi_tm_thread_main(void *arg)
{
	struct timer_list *t;
	void (*fn)(struct timer_list *);
	unsigned long now;

	mutex_enter(&lkpi_tm_lock);
	lkpi_tm_thread = curthread;
	while (!lkpi_tm_exiting) {
		t = avl_first(&lkpi_tm_tree);
		if (t == NULL) {
			cv_wait(&lkpi_tm_cv, &lkpi_tm_lock);
			continue;
		}
		now = lkpi_jiffies();
		if (now < t->expires) {
			(void) cv_reltimedwait(&lkpi_tm_cv, &lkpi_tm_lock,
			    lkpi_sleep_ticks(t->expires - now), TR_CLOCK_TICK);
			continue;
		}
		lkpi_tm_remove_locked(t);
		fn = t->function;
		lkpi_tm_running = t;
		mutex_exit(&lkpi_tm_lock);

		if (fn != NULL)
			fn(t);
		else
			WARN_ONCE(1, "linuxkpi: timer without a function");

		mutex_enter(&lkpi_tm_lock);
		lkpi_tm_running = NULL;
		cv_broadcast(&lkpi_tm_done_cv);
	}
	lkpi_tm_thread = NULL;
	mutex_exit(&lkpi_tm_lock);
}

/* --- hrtimers ------------------------------------------------------------- */

/* The clock's current time, in nanoseconds. */
static ktime_t
lkpi_hrtimer_now(int clock)
{
	return (clock == CLOCK_REALTIME ? ktime_get_real() : ktime_get());
}

/* The jiffy at which an expiry on the clock is reached. */
static unsigned long
lkpi_hrtimer_jiffies(int clock, ktime_t expires)
{
	s64 mono = expires;

	if (clock == CLOCK_REALTIME)
		mono = expires - (ktime_get_real() - ktime_get());
	if (mono <= 0)
		return (0);
	return ((unsigned long)((mono + (NSEC_PER_SEC / HZ) - 1) /
	    (NSEC_PER_SEC / HZ)));
}

static void
lkpi_hrtimer_fn(struct timer_list *tl)
{
	struct hrtimer *t = container_of(tl, struct hrtimer, timer);
	enum hrtimer_restart (*fn)(struct hrtimer *) = t->function;

	/* With NORESTART the function may free t: touch t only to re-arm. */
	t->active = 0;
	if (fn == NULL) {
		WARN_ONCE(1, "linuxkpi: hrtimer without a function");
		return;
	}
	if (fn(t) == HRTIMER_RESTART) {
		mutex_enter(&lkpi_tm_lock);
		/* A concurrent hrtimer_start() armed it already. */
		if (!lkpi_tm_pending(&t->timer)) {
			t->active = 1;
			(void) lkpi_tm_arm_locked(&t->timer,
			    lkpi_hrtimer_jiffies(t->clock, t->expires));
		}
		mutex_exit(&lkpi_tm_lock);
	}
}

void
hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *),
    int clock, enum hrtimer_mode mode)
{
	lkpi_timer_setup(&t->timer, lkpi_hrtimer_fn, 0);
	t->expires = 0;
	t->function = fn;
	t->clock = clock;
	t->active = 0;
}

void
hrtimer_start_range_ns(struct hrtimer *t, ktime_t tim, u64 delta_ns,
    const enum hrtimer_mode mode)
{
	mutex_enter(&lkpi_tm_lock);
	if (mode & HRTIMER_MODE_REL)
		tim = ktime_add(lkpi_hrtimer_now(t->clock), tim);
	t->expires = tim;
	t->active = 1;
	(void) lkpi_tm_arm_locked(&t->timer,
	    lkpi_hrtimer_jiffies(t->clock, tim));
	mutex_exit(&lkpi_tm_lock);
}

/* 1 if t was pending, 0 if idle, -1 if its function is running. */
int
hrtimer_try_to_cancel(struct hrtimer *t)
{
	int ret = 0;

	mutex_enter(&lkpi_tm_lock);
	if (lkpi_tm_running == &t->timer) {
		ret = -1;
	} else if (lkpi_tm_pending(&t->timer)) {
		lkpi_tm_remove_locked(&t->timer);
		t->active = 0;
		ret = 1;
	}
	mutex_exit(&lkpi_tm_lock);
	return (ret);
}

int
hrtimer_cancel(struct hrtimer *t)
{
	int was = timer_delete_sync(&t->timer);

	t->active = 0;
	return (was);
}

bool
hrtimer_active(const struct hrtimer *t)
{
	bool active;

	mutex_enter(&lkpi_tm_lock);
	active = lkpi_tm_pending(&t->timer) || lkpi_tm_running == &t->timer;
	mutex_exit(&lkpi_tm_lock);
	return (active);
}

/*
 * Moves the expiry forward by whole intervals until it is after `now`;
 * returns the number of intervals (0 if it already was).
 */
u64
hrtimer_forward(struct hrtimer *t, ktime_t now, ktime_t interval)
{
	s64 delta = now - t->expires;
	u64 n;

	if (delta < 0)
		return (0);
	if (interval <= 0)
		interval = 1;
	n = (u64)(delta / interval) + 1;
	t->expires += (s64)n * interval;
	return (n);
}

/* --- module state ---------------------------------------------------------- */

int
lkpi_timer_init(void)
{
	(mutex_init)(&lkpi_tm_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&lkpi_tm_cv, NULL, CV_DRIVER, NULL);
	cv_init(&lkpi_tm_done_cv, NULL, CV_DRIVER, NULL);
	avl_create(&lkpi_tm_tree, lkpi_tm_compare, sizeof (struct timer_list),
	    offsetof(struct timer_list, lkpi_node));
	lkpi_tm_exiting = B_FALSE;
	lkpi_tm_running = NULL;

	lkpi_tm_tq = ddi_taskq_create(NULL, "lkpi_timer", 1, LKPI_PRI_HIGH, 0);
	if (lkpi_tm_tq != NULL && ddi_taskq_dispatch(lkpi_tm_tq,
	    lkpi_tm_thread_main, NULL, DDI_SLEEP) == DDI_SUCCESS)
		return (0);

	if (lkpi_tm_tq != NULL)
		ddi_taskq_destroy(lkpi_tm_tq);
	lkpi_tm_tq = NULL;
	avl_destroy(&lkpi_tm_tree);
	cv_destroy(&lkpi_tm_done_cv);
	cv_destroy(&lkpi_tm_cv);
	(mutex_destroy)(&lkpi_tm_lock);
	return (-ENOMEM);
}

void
lkpi_timer_fini(void)
{
	struct timer_list *t;
	void *cookie = NULL;
	ulong_t left;

	mutex_enter(&lkpi_tm_lock);
	lkpi_tm_exiting = B_TRUE;
	cv_broadcast(&lkpi_tm_cv);
	mutex_exit(&lkpi_tm_lock);
	ddi_taskq_destroy(lkpi_tm_tq);

	/* Timers still armed at unload never fire. */
	mutex_enter(&lkpi_tm_lock);
	lkpi_tm_tq = NULL;
	left = avl_numnodes(&lkpi_tm_tree);
	while ((t = avl_destroy_nodes(&lkpi_tm_tree, &cookie)) != NULL)
		t->lkpi_seq = 0;
	mutex_exit(&lkpi_tm_lock);
	if (left != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu timers pending at unload",
		    left);

	avl_destroy(&lkpi_tm_tree);
	cv_destroy(&lkpi_tm_done_cv);
	cv_destroy(&lkpi_tm_cv);
	(mutex_destroy)(&lkpi_tm_lock);
}
