/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's workqueues, irq_work and tasklets (<linux/workqueue.h>,
 * <linux/irq_work.h>, the tasklet part of <linux/interrupt.h>).
 *
 * A workqueue is a list of pending items served by worker threads: the
 * threads of a ddi_taskq(9F), each running one long-lived loop task
 * dispatched when the queue is created. All workqueue state is protected
 * by one layer mutex (lkpi_wq_lock), so an item's state is consistent
 * even when it moves between queues; the critical sections are a few list
 * operations. queue_work() links the item and signals a worker: it never
 * allocates and works in interrupt handlers.
 *
 * Item state lives in work_struct.data (bits below) and in the records of
 * the workers that execute items: an item is running while a worker's
 * record points at it. The worker never dereferences the item after its
 * function returns (the function may free it), so "running" is never a
 * bit in the item. As on Linux, PENDING is cleared before the function
 * runs (the function may requeue the item), and an item never runs on two
 * workers at once: workers skip pending items that are running elsewhere
 * and come back to them when the run ends. Every item taken from a queue
 * or cancelled advances its generation (the upper bits of data), which
 * lets flush_work() wait for exactly the queued instance.
 *
 * flush_workqueue() uses two flush colours per queue: items take the
 * queue's current colour when queued, and a flush flips the colour and
 * waits until no item of the old colour is pending or running, so items
 * queued after the flush started do not hold it up.
 *
 * Delayed work arms the item's layer timer (src/lkpi_timer.c), whose
 * function queues the item if it is still the armed instance.
 *
 * irq_work and tasklets run on two layer workqueues of high priority
 * (thread context, not interrupt context).
 */

#include <linux/workqueue.h>
#include <linux/irq_work.h>
#include <linux/interrupt.h>
#include <linux/wait_bit.h>
#include <linux/delay.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/bug.h>
#include <asm/barrier.h>
#include "lkpi_internal.h"

/* work_struct.data */
#define	LKPI_WORK_PENDING	(1UL << WORK_STRUCT_PENDING_BIT)
#define	LKPI_WORK_DELAYED	(1UL << 2)	/* the delayed-work timer is armed */
#define	LKPI_WORK_CANCELING	(1UL << 3)	/* cancel_*_sync(): no queueing */
#define	LKPI_WORK_COLOR		(1UL << 4)	/* flush colour while pending */
#define	LKPI_WORK_RCU		(1UL << 5)	/* queue_rcu_work(): in RCU */
#define	LKPI_WORK_GEN_SHIFT	8
#define	LKPI_WORK_GEN_ONE	(1UL << LKPI_WORK_GEN_SHIFT)

#define	LKPI_WQ_MAX_THREADS	8
#define	LKPI_WQ_DFL_THREADS	4

struct workqueue_struct {
	char name[32];
	unsigned int flags;
	int nthreads;
	ddi_taskq_t *tq;
	struct list_head pending;
	kcondvar_t cv;			/* workers: new or runnable items */
	unsigned int color;		/* colour of newly queued items */
	unsigned long inflight[2];	/* pending or running, per colour */
	unsigned long ndelayed;		/* delayed items with armed timers */
	kmutex_t flush_lock;		/* serialises flush_workqueue() */
	boolean_t exiting;
};

/* A worker thread executing an item (on the worker's stack). */
struct lkpi_worker {
	struct list_head link;
	struct workqueue_struct *wq;
	struct work_struct *cur;
	kthread_t *thread;
};

static kmutex_t lkpi_wq_lock;
static kcondvar_t lkpi_wq_done_cv;	/* an item ran or was cancelled */
static struct list_head lkpi_wq_running = LIST_HEAD_INIT(lkpi_wq_running);

struct workqueue_struct *system_wq;
struct workqueue_struct *system_percpu_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_dfl_wq;
struct workqueue_struct *system_freezable_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_freezable_power_efficient_wq;

static struct workqueue_struct *lkpi_irq_work_wq;
static struct workqueue_struct *lkpi_tasklet_wq;

/* --- item state (lkpi_wq_lock held for changes) ------------------------- */

static inline unsigned long
wd_get(const struct work_struct *w)
{
	return ((unsigned long)__atomic_load_n(&w->data.counter,
	    __ATOMIC_SEQ_CST));
}

static inline void
wd_set(struct work_struct *w, unsigned long v)
{
	__atomic_store_n(&w->data.counter, (long)v, __ATOMIC_SEQ_CST);
}

static inline void
wd_or(struct work_struct *w, unsigned long bits)
{
	wd_set(w, wd_get(w) | bits);
}

static inline void
wd_clear(struct work_struct *w, unsigned long bits)
{
	wd_set(w, wd_get(w) & ~bits);
}

static inline unsigned long
wd_gen(const struct work_struct *w)
{
	return (wd_get(w) >> LKPI_WORK_GEN_SHIFT);
}

/* The worker executing w, if any. */
static struct lkpi_worker *
lkpi_wq_runner_locked(const struct work_struct *w)
{
	struct lkpi_worker *wk;

	list_for_each_entry(wk, &lkpi_wq_running, link) {
		if (wk->cur == w)
			return (wk);
	}
	return (NULL);
}

/* The calling thread's worker record, if it executes an item. */
static struct lkpi_worker *
lkpi_wq_self_locked(void)
{
	struct lkpi_worker *wk;

	list_for_each_entry(wk, &lkpi_wq_running, link) {
		if (wk->thread == curthread)
			return (wk);
	}
	return (NULL);
}

/* Links a pending item to wq in the queue's current colour. */
static void
lkpi_wq_insert_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	unsigned long d = wd_get(w) & ~LKPI_WORK_COLOR;

	if (wq->color != 0)
		d |= LKPI_WORK_COLOR;
	wd_set(w, d);
	w->wq = wq;
	wq->inflight[wq->color]++;
	list_add_tail(&w->entry, &wq->pending);
	cv_signal(&wq->cv);
}

static boolean_t
lkpi_wq_usable(struct workqueue_struct *wq)
{
	if (wq == NULL || wq->exiting) {
		WARN_ONCE(1, "linuxkpi: work queued to a %s workqueue",
		    wq == NULL ? "missing" : "destroyed");
		return (B_FALSE);
	}
	return (B_TRUE);
}

static bool
lkpi_queue_work_locked(struct workqueue_struct *wq, struct work_struct *w)
{
	if ((wd_get(w) & (LKPI_WORK_PENDING | LKPI_WORK_CANCELING)) != 0)
		return (false);
	if (!lkpi_wq_usable(wq))
		return (false);
	wd_or(w, LKPI_WORK_PENDING);
	lkpi_wq_insert_locked(wq, w);
	return (true);
}

/*
 * Takes a pending item off its queue or disarms its delayed-work timer;
 * returns whether it did. The generation advances, as for an
 * item that ran. A delayed-work timer function already past the tree
 * finds DELAYED clear and does nothing.
 */
static bool
lkpi_work_grab_locked(struct work_struct *w)
{
	unsigned long d = wd_get(w);
	struct delayed_work *dw;
	struct workqueue_struct *wq;

	/* An rcu_work waiting for its grace period cannot be taken back. */
	if ((d & LKPI_WORK_PENDING) == 0 || (d & LKPI_WORK_RCU) != 0)
		return (false);
	if ((d & LKPI_WORK_DELAYED) != 0) {
		dw = to_delayed_work(w);
		(void) timer_delete(&dw->timer);
		if (dw->wq != NULL && dw->wq->ndelayed > 0)
			dw->wq->ndelayed--;
	} else {
		wq = w->wq;
		list_del_init(&w->entry);
		wq->inflight[(d & LKPI_WORK_COLOR) != 0]--;
		cv_broadcast(&lkpi_wq_done_cv);
	}
	wd_set(w, (d & ~(LKPI_WORK_PENDING | LKPI_WORK_DELAYED)) +
	    LKPI_WORK_GEN_ONE);
	return (true);
}

/* Waits until w does not run, unless the caller is running it. */
static void
lkpi_work_wait_idle_locked(struct work_struct *w)
{
	struct lkpi_worker *wk;

	while ((wk = lkpi_wq_runner_locked(w)) != NULL) {
		if (wk->thread == curthread)
			return;
		cv_wait(&lkpi_wq_done_cv, &lkpi_wq_lock);
	}
}

static boolean_t
lkpi_work_may_wait(void)
{
	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: waiting for work in an interrupt");
		return (B_FALSE);
	}
	return (B_TRUE);
}

/* --- worker threads ------------------------------------------------------ */

/* The first item a worker may run now (ordered queues: only the head). */
static struct work_struct *
lkpi_wq_next_locked(struct workqueue_struct *wq)
{
	struct work_struct *w;

	list_for_each_entry(w, &wq->pending, entry) {
		if (lkpi_wq_runner_locked(w) == NULL)
			return (w);
		if (wq->nthreads == 1)
			break;
	}
	return (NULL);
}

static void
lkpi_wq_worker(void *arg)
{
	struct workqueue_struct *wq = arg;
	struct lkpi_worker me;
	struct work_struct *w;
	work_func_t func;
	unsigned long d;
	int color;

	me.wq = wq;
	me.cur = NULL;
	me.thread = curthread;
	mutex_enter(&lkpi_wq_lock);
	for (;;) {
		w = lkpi_wq_next_locked(wq);
		if (w == NULL) {
			if (wq->exiting && list_empty(&wq->pending))
				break;
			/* Items running elsewhere: wait for a run to end. */
			cv_wait(list_empty(&wq->pending) ? &wq->cv :
			    &lkpi_wq_done_cv, &lkpi_wq_lock);
			continue;
		}
		list_del_init(&w->entry);
		d = wd_get(w);
		color = (d & LKPI_WORK_COLOR) != 0;
		wd_set(w, (d & ~LKPI_WORK_PENDING) + LKPI_WORK_GEN_ONE);
		func = w->func;
		me.cur = w;
		list_add(&me.link, &lkpi_wq_running);
		mutex_exit(&lkpi_wq_lock);

		if (func != NULL)
			func(w);
		else
			WARN_ONCE(1, "linuxkpi: work item without a function");

		mutex_enter(&lkpi_wq_lock);
		list_del(&me.link);
		me.cur = NULL;
		wq->inflight[color]--;
		cv_broadcast(&lkpi_wq_done_cv);
	}
	mutex_exit(&lkpi_wq_lock);
}

/* --- workqueues ------------------------------------------------------------ */

static void
lkpi_wq_free(struct workqueue_struct *wq)
{
	cv_destroy(&wq->cv);
	(mutex_destroy)(&wq->flush_lock);
	kmem_free(wq, sizeof (*wq));
}

struct workqueue_struct *
lkpi_alloc_workqueue(const char *name, unsigned int flags, int max_active)
{
	struct workqueue_struct *wq;
	int i, n;

	if (max_active == 1 || (flags & __WQ_ORDERED) != 0)
		n = 1;
	else if (max_active <= 0)
		n = LKPI_WQ_DFL_THREADS;
	else
		n = MIN(max_active, LKPI_WQ_MAX_THREADS);

	wq = kmem_zalloc(sizeof (*wq), KM_SLEEP);
	(void) strlcpy(wq->name, name != NULL ? name : "lkpi_wq",
	    sizeof (wq->name));
	wq->flags = flags;
	wq->nthreads = n;
	INIT_LIST_HEAD(&wq->pending);
	cv_init(&wq->cv, NULL, CV_DRIVER, NULL);
	(mutex_init)(&wq->flush_lock, NULL, MUTEX_DRIVER, NULL);

	wq->tq = ddi_taskq_create(NULL, wq->name, n,
	    (flags & WQ_HIGHPRI) != 0 ? LKPI_PRI_HIGH : TASKQ_DEFAULTPRI, 0);
	if (wq->tq == NULL) {
		lkpi_wq_free(wq);
		return (NULL);
	}
	for (i = 0; i < n; i++) {
		if (ddi_taskq_dispatch(wq->tq, lkpi_wq_worker, wq,
		    DDI_SLEEP) != DDI_SUCCESS)
			break;
	}
	if (i < n) {
		mutex_enter(&lkpi_wq_lock);
		wq->exiting = B_TRUE;
		cv_broadcast(&wq->cv);
		mutex_exit(&lkpi_wq_lock);
		ddi_taskq_destroy(wq->tq);
		lkpi_wq_free(wq);
		return (NULL);
	}
	return (wq);
}

struct workqueue_struct *
alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
	char name[32];
	va_list ap;

	va_start(ap, max_active);
	(void) vsnprintf(name, sizeof (name), fmt, ap);
	va_end(ap);
	return (lkpi_alloc_workqueue(name, flags, max_active));
}

void
flush_workqueue(struct workqueue_struct *wq)
{
	struct lkpi_worker *self;
	unsigned int c;

	if (wq == NULL || !lkpi_work_may_wait())
		return;
	mutex_enter(&wq->flush_lock);
	mutex_enter(&lkpi_wq_lock);
	self = lkpi_wq_self_locked();
	if (self != NULL && self->wq == wq) {
		/* Linux deadlocks here; the item itself never finishes. */
		WARN_ONCE(1, "linuxkpi: %s flushed by its own work item",
		    wq->name);
	} else {
		c = wq->color;
		wq->color ^= 1;
		while (wq->inflight[c] != 0)
			cv_wait(&lkpi_wq_done_cv, &lkpi_wq_lock);
	}
	mutex_exit(&lkpi_wq_lock);
	mutex_exit(&wq->flush_lock);
}

/* Flushes until the queue is empty, including items queued meanwhile. */
void
drain_workqueue(struct workqueue_struct *wq)
{
	boolean_t idle;

	if (wq == NULL)
		return;
	for (;;) {
		flush_workqueue(wq);
		mutex_enter(&lkpi_wq_lock);
		idle = wq->inflight[0] == 0 && wq->inflight[1] == 0;
		mutex_exit(&lkpi_wq_lock);
		if (idle || !lkpi_work_may_wait())
			break;
	}
}

void
destroy_workqueue(struct workqueue_struct *wq)
{
	if (wq == NULL)
		return;
	drain_workqueue(wq);
	mutex_enter(&lkpi_wq_lock);
	if (wq->ndelayed != 0)
		cmn_err(CE_WARN, "linuxkpi: workqueue %s destroyed with %lu "
		    "delayed items armed", wq->name, wq->ndelayed);
	wq->exiting = B_TRUE;
	cv_broadcast(&wq->cv);
	mutex_exit(&lkpi_wq_lock);
	/* Waits for the workers, which run what is still pending first. */
	ddi_taskq_destroy(wq->tq);
	lkpi_wq_free(wq);
}

/* --- work items ------------------------------------------------------------ */

void
lkpi_init_work(struct work_struct *w, work_func_t func)
{
	w->data.counter = 0;
	INIT_LIST_HEAD(&w->entry);
	w->func = func;
	w->wq = NULL;
	w->lockdep_map.name = NULL;
}

bool
queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
	bool queued;

	mutex_enter(&lkpi_wq_lock);
	queued = lkpi_queue_work_locked(wq, w);
	mutex_exit(&lkpi_wq_lock);
	return (queued);
}

bool
cancel_work(struct work_struct *w)
{
	bool was;

	mutex_enter(&lkpi_wq_lock);
	was = lkpi_work_grab_locked(w);
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

bool
cancel_work_sync(struct work_struct *w)
{
	bool was;

	if (!lkpi_work_may_wait())
		return (cancel_work(w));
	mutex_enter(&lkpi_wq_lock);
	wd_or(w, LKPI_WORK_CANCELING);
	was = lkpi_work_grab_locked(w);
	lkpi_work_wait_idle_locked(w);
	wd_clear(w, LKPI_WORK_CANCELING);
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

/*
 * Waits until the instance of w queued at the call (or running, if none
 * is queued) has finished; false if w was idle. A delayed item whose
 * timer is armed is not queued yet and is not waited for, as on Linux.
 */
bool
flush_work(struct work_struct *w)
{
	struct lkpi_worker *wk;
	unsigned long d, target;

	if (!lkpi_work_may_wait())
		return (false);
	mutex_enter(&lkpi_wq_lock);
	d = wd_get(w);
	wk = lkpi_wq_runner_locked(w);
	if ((d & (LKPI_WORK_PENDING | LKPI_WORK_DELAYED)) ==
	    LKPI_WORK_PENDING) {
		target = wd_gen(w) + 1;
	} else if (wk != NULL && wk->thread != curthread) {
		target = wd_gen(w);
	} else {
		mutex_exit(&lkpi_wq_lock);
		return (false);
	}
	for (;;) {
		wk = lkpi_wq_runner_locked(w);
		if ((long)(wd_gen(w) - target) > 0 ||
		    (wd_gen(w) == target && wk == NULL))
			break;
		if (wk != NULL && wk->thread == curthread) {
			WARN_ONCE(1, "linuxkpi: work item flushes itself");
			break;
		}
		cv_wait(&lkpi_wq_done_cv, &lkpi_wq_lock);
	}
	mutex_exit(&lkpi_wq_lock);
	return (true);
}

bool
work_busy(struct work_struct *w)
{
	bool busy;

	mutex_enter(&lkpi_wq_lock);
	busy = (wd_get(w) & LKPI_WORK_PENDING) != 0 ||
	    lkpi_wq_runner_locked(w) != NULL;
	mutex_exit(&lkpi_wq_lock);
	return (busy);
}

struct work_struct *
current_work(void)
{
	struct lkpi_worker *wk;
	struct work_struct *w;

	mutex_enter(&lkpi_wq_lock);
	wk = lkpi_wq_self_locked();
	w = wk != NULL ? wk->cur : NULL;
	mutex_exit(&lkpi_wq_lock);
	return (w);
}

/* --- delayed work ------------------------------------------------------------ */

/*
 * The delayed-work timer: queue the item if this arming is still the
 * current one (DELAYED set, timer not re-armed meanwhile).
 */
static void
lkpi_delayed_work_timer(struct timer_list *t)
{
	struct delayed_work *dw = container_of(t, struct delayed_work, timer);
	struct workqueue_struct *wq;

	mutex_enter(&lkpi_wq_lock);
	if ((wd_get(&dw->work) & LKPI_WORK_DELAYED) != 0 &&
	    !timer_pending(&dw->timer)) {
		wd_clear(&dw->work, LKPI_WORK_DELAYED);
		wq = dw->wq;
		if (wq->ndelayed > 0)
			wq->ndelayed--;
		if (wq->exiting) {
			WARN_ONCE(1, "linuxkpi: delayed work fired for a "
			    "destroyed workqueue");
			wd_set(&dw->work, (wd_get(&dw->work) &
			    ~LKPI_WORK_PENDING) + LKPI_WORK_GEN_ONE);
			cv_broadcast(&lkpi_wq_done_cv);
		} else {
			lkpi_wq_insert_locked(wq, &dw->work);
		}
	}
	mutex_exit(&lkpi_wq_lock);
}

void
lkpi_init_delayed_work(struct delayed_work *dw, work_func_t func)
{
	lkpi_init_work(&dw->work, func);
	lkpi_timer_setup(&dw->timer, lkpi_delayed_work_timer, 0);
	dw->wq = NULL;
	dw->cpu = 0;
}

/* Queues a non-pending delayed item now or arms its timer. */
static void
lkpi_queue_delayed_locked(struct workqueue_struct *wq, struct delayed_work *dw,
    unsigned long delay)
{
	/* DECLARE_DELAYED_WORK() leaves the timer zero-filled. */
	if (dw->timer.function == NULL)
		lkpi_timer_setup(&dw->timer, lkpi_delayed_work_timer, 0);
	wd_or(&dw->work, LKPI_WORK_PENDING);
	dw->wq = wq;
	if (delay == 0) {
		lkpi_wq_insert_locked(wq, &dw->work);
		return;
	}
	wd_or(&dw->work, LKPI_WORK_DELAYED);
	dw->work.wq = wq;
	wq->ndelayed++;
	(void) mod_timer(&dw->timer, lkpi_jiffies() + delay);
}

bool
queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
    unsigned long delay)
{
	bool queued = false;

	mutex_enter(&lkpi_wq_lock);
	if ((wd_get(&dw->work) & (LKPI_WORK_PENDING | LKPI_WORK_CANCELING)) ==
	    0 && lkpi_wq_usable(wq)) {
		lkpi_queue_delayed_locked(wq, dw, delay);
		queued = true;
	}
	mutex_exit(&lkpi_wq_lock);
	return (queued);
}

/* Re-queues dw for `delay` from now, pending or not; true if it was. */
bool
mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
    unsigned long delay)
{
	bool was = false;

	mutex_enter(&lkpi_wq_lock);
	if ((wd_get(&dw->work) & LKPI_WORK_CANCELING) == 0 &&
	    lkpi_wq_usable(wq)) {
		was = lkpi_work_grab_locked(&dw->work);
		lkpi_queue_delayed_locked(wq, dw, delay);
	}
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

bool
flush_delayed_work(struct delayed_work *dw)
{
	mutex_enter(&lkpi_wq_lock);
	if ((wd_get(&dw->work) & LKPI_WORK_DELAYED) != 0) {
		(void) timer_delete(&dw->timer);
		wd_clear(&dw->work, LKPI_WORK_DELAYED);
		if (dw->wq->ndelayed > 0)
			dw->wq->ndelayed--;
		lkpi_wq_insert_locked(dw->wq, &dw->work);
	}
	mutex_exit(&lkpi_wq_lock);
	return (flush_work(&dw->work));
}

/*
 * Cancels a pending dw. If its timer function is already past the timer
 * tree it does nothing, but it still reads dw: outside interrupts, wait
 * until it returned, so the caller may free dw afterwards.
 */
bool
cancel_delayed_work(struct delayed_work *dw)
{
	boolean_t delayed;
	bool was;

	mutex_enter(&lkpi_wq_lock);
	delayed = (wd_get(&dw->work) & LKPI_WORK_DELAYED) != 0;
	was = lkpi_work_grab_locked(&dw->work);
	mutex_exit(&lkpi_wq_lock);
	if (delayed && !servicing_interrupt())
		lkpi_timer_wait_idle(&dw->timer);
	return (was);
}

bool
cancel_delayed_work_sync(struct delayed_work *dw)
{
	boolean_t delayed;
	bool was;

	if (!lkpi_work_may_wait())
		return (cancel_delayed_work(dw));
	mutex_enter(&lkpi_wq_lock);
	wd_or(&dw->work, LKPI_WORK_CANCELING);
	delayed = (wd_get(&dw->work) & LKPI_WORK_DELAYED) != 0;
	was = lkpi_work_grab_locked(&dw->work);
	mutex_exit(&lkpi_wq_lock);
	if (delayed)
		lkpi_timer_wait_idle(&dw->timer);
	mutex_enter(&lkpi_wq_lock);
	lkpi_work_wait_idle_locked(&dw->work);
	wd_clear(&dw->work, LKPI_WORK_CANCELING);
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

/* --- RCU work ---------------------------------------------------------------- */

static void
lkpi_rcu_work_rcufn(struct rcu_head *head)
{
	struct rcu_work *rw = container_of(head, struct rcu_work, rcu);

	mutex_enter(&lkpi_wq_lock);
	wd_clear(&rw->work, LKPI_WORK_RCU);
	if (lkpi_wq_usable(rw->wq)) {
		lkpi_wq_insert_locked(rw->wq, &rw->work);
	} else {
		wd_set(&rw->work, (wd_get(&rw->work) & ~LKPI_WORK_PENDING) +
		    LKPI_WORK_GEN_ONE);
		cv_broadcast(&lkpi_wq_done_cv);
	}
	mutex_exit(&lkpi_wq_lock);
}

/* Queues rw after a grace period; the item is pending meanwhile. */
bool
queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rw)
{
	mutex_enter(&lkpi_wq_lock);
	if ((wd_get(&rw->work) & (LKPI_WORK_PENDING | LKPI_WORK_CANCELING)) !=
	    0 || !lkpi_wq_usable(wq)) {
		mutex_exit(&lkpi_wq_lock);
		return (false);
	}
	wd_or(&rw->work, LKPI_WORK_PENDING | LKPI_WORK_RCU);
	rw->wq = wq;
	mutex_exit(&lkpi_wq_lock);
	call_rcu(&rw->rcu, lkpi_rcu_work_rcufn);
	return (true);
}

/* --- irq_work ------------------------------------------------------------------ */

static void
lkpi_irq_work_fn(struct work_struct *w)
{
	struct irq_work *iw = container_of(w, struct irq_work, work);

	iw->func(iw);
}

void
init_irq_work(struct irq_work *w, void (*func)(struct irq_work *))
{
	bzero(w, sizeof (*w));
	w->func = func;
	lkpi_init_work(&w->work, lkpi_irq_work_fn);
}

/* Initialises the embedded item of a statically initialised struct. */
static void
lkpi_work_init_once_locked(struct work_struct *w, work_func_t func)
{
	if (w->func == NULL) {
		INIT_LIST_HEAD(&w->entry);
		w->func = func;
	}
}

bool
irq_work_queue(struct irq_work *w)
{
	bool queued;

	mutex_enter(&lkpi_wq_lock);
	lkpi_work_init_once_locked(&w->work, lkpi_irq_work_fn);
	queued = lkpi_queue_work_locked(lkpi_irq_work_wq, &w->work);
	mutex_exit(&lkpi_wq_lock);
	return (queued);
}

void
irq_work_sync(struct irq_work *w)
{
	if (w->work.func != NULL)
		(void) flush_work(&w->work);
}

/* --- tasklets -------------------------------------------------------------------- */

/*
 * t->state: Linux's SCHED and RUN bits, plus KILL while tasklet_kill()
 * holds SCHED (the queued item must then not run the tasklet). The item
 * t->lkpi_work runs the tasklet on the layer's tasklet workqueue; it is
 * queued by tasklet_schedule(), by tasklet_enable() and tasklet_unlock()
 * for a tasklet scheduled meanwhile, and by itself (after a tick) while
 * the tasklet is disabled by a driver that bypasses tasklet_enable().
 */
#define	LKPI_TASKLET_SCHED	(1UL << TASKLET_STATE_SCHED)
#define	LKPI_TASKLET_RUN	(1UL << TASKLET_STATE_RUN)
#define	LKPI_TASKLET_KILL	(1UL << 2)

static void lkpi_tasklet_work(struct work_struct *w);

static inline unsigned long
ts_get(struct tasklet_struct *t)
{
	return (__atomic_load_n(&t->state, __ATOMIC_SEQ_CST));
}

static void
lkpi_tasklet_queue(struct tasklet_struct *t)
{
	mutex_enter(&lkpi_wq_lock);
	lkpi_work_init_once_locked(&t->lkpi_work, lkpi_tasklet_work);
	(void) lkpi_queue_work_locked(lkpi_tasklet_wq, &t->lkpi_work);
	mutex_exit(&lkpi_wq_lock);
}

/* Queues t if it is scheduled, enabled and not being killed. */
static void
lkpi_tasklet_kick(struct tasklet_struct *t)
{
	if ((ts_get(t) & (LKPI_TASKLET_SCHED | LKPI_TASKLET_KILL)) ==
	    LKPI_TASKLET_SCHED && atomic_read(&t->count) == 0)
		lkpi_tasklet_queue(t);
}

static void
lkpi_tasklet_work(struct work_struct *w)
{
	struct tasklet_struct *t = container_of(w, struct tasklet_struct,
	    lkpi_work);
	unsigned long s;

	if ((ts_get(t) & LKPI_TASKLET_KILL) != 0)
		return;
	/* Run elsewhere (tasklet_trylock()): its unlock queues us again. */
	if (!tasklet_trylock(t))
		return;
	if (atomic_read(&t->count) != 0) {
		/*
		 * Disabled: poll every tick, as Linux's softirq retries a
		 * disabled tasklet (i915 enables with a bare atomic decrement,
		 * which does not call tasklet_enable()).
		 */
		tasklet_unlock(t);
		delay(1);
		if ((ts_get(t) & (LKPI_TASKLET_SCHED | LKPI_TASKLET_KILL)) ==
		    LKPI_TASKLET_SCHED)
			lkpi_tasklet_queue(t);
		return;
	}
	/* Take SCHED unless tasklet_kill() holds it. */
	s = ts_get(t);
	do {
		if ((s & (LKPI_TASKLET_SCHED | LKPI_TASKLET_KILL)) !=
		    LKPI_TASKLET_SCHED) {
			tasklet_unlock(t);
			return;
		}
	} while (!__atomic_compare_exchange_n(&t->state, &s,
	    s & ~LKPI_TASKLET_SCHED, B_FALSE, __ATOMIC_SEQ_CST,
	    __ATOMIC_SEQ_CST));
	wake_up_var(&t->state);

	if (t->use_callback)
		t->callback(t);
	else
		t->func(t->data);
	tasklet_unlock(t);
}

void
tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
    unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->use_callback = false;
	t->func = func;
	t->data = data;
	lkpi_init_work(&t->lkpi_work, lkpi_tasklet_work);
}

void
tasklet_setup(struct tasklet_struct *t,
    void (*callback)(struct tasklet_struct *))
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->use_callback = true;
	t->callback = callback;
	t->data = 0;
	lkpi_init_work(&t->lkpi_work, lkpi_tasklet_work);
}

void
tasklet_schedule(struct tasklet_struct *t)
{
	if ((__atomic_fetch_or(&t->state, LKPI_TASKLET_SCHED,
	    __ATOMIC_SEQ_CST) & LKPI_TASKLET_SCHED) != 0)
		return;
	/* A disabled tasklet stays scheduled until tasklet_enable(). */
	lkpi_tasklet_kick(t);
}

void
tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	if (atomic_dec_and_test(&t->count))
		lkpi_tasklet_kick(t);
}

bool
tasklet_trylock(struct tasklet_struct *t)
{
	return ((__atomic_fetch_or(&t->state, LKPI_TASKLET_RUN,
	    __ATOMIC_SEQ_CST) & LKPI_TASKLET_RUN) == 0);
}

void
tasklet_unlock(struct tasklet_struct *t)
{
	(void) __atomic_fetch_and(&t->state, ~LKPI_TASKLET_RUN,
	    __ATOMIC_SEQ_CST);
	wake_up_var(&t->state);
	/* Scheduled while someone else held RUN: run it now. */
	lkpi_tasklet_kick(t);
}

void
tasklet_unlock_wait(struct tasklet_struct *t)
{
	if (servicing_interrupt()) {
		tasklet_unlock_spin_wait(t);
		return;
	}
	wait_var_event(&t->state, (ts_get(t) & LKPI_TASKLET_RUN) == 0);
}

void
tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
	while ((ts_get(t) & LKPI_TASKLET_RUN) != 0)
		cpu_relax();
}

/*
 * Waits until a scheduled run has happened, then takes SCHED (with KILL)
 * so that the tasklet cannot be scheduled, removes a stray queued item,
 * waits for a running callback and leaves the tasklet idle.
 */
void
tasklet_kill(struct tasklet_struct *t)
{
	unsigned long s;

	if (!lkpi_work_may_wait())
		return;
	for (;;) {
		s = ts_get(t);
		if ((s & LKPI_TASKLET_SCHED) != 0) {
			wait_var_event(&t->state,
			    (ts_get(t) & LKPI_TASKLET_SCHED) == 0);
			continue;
		}
		if (__atomic_compare_exchange_n(&t->state, &s,
		    s | LKPI_TASKLET_SCHED | LKPI_TASKLET_KILL, B_FALSE,
		    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			break;
	}
	if (t->lkpi_work.func != NULL)
		(void) cancel_work_sync(&t->lkpi_work);
	tasklet_unlock_wait(t);
	(void) __atomic_fetch_and(&t->state,
	    ~(LKPI_TASKLET_SCHED | LKPI_TASKLET_KILL), __ATOMIC_SEQ_CST);
	wake_up_var(&t->state);
}

/* --- module state ------------------------------------------------------------- */

int
lkpi_work_init(void)
{
	(mutex_init)(&lkpi_wq_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&lkpi_wq_done_cv, NULL, CV_DRIVER, NULL);
	INIT_LIST_HEAD(&lkpi_wq_running);

	system_wq = lkpi_alloc_workqueue("lkpi_events", 0, 8);
	system_highpri_wq = lkpi_alloc_workqueue("lkpi_events_highpri",
	    WQ_HIGHPRI, 4);
	system_long_wq = lkpi_alloc_workqueue("lkpi_events_long", 0, 4);
	system_unbound_wq = lkpi_alloc_workqueue("lkpi_events_unbound",
	    WQ_UNBOUND, 8);
	system_freezable_wq = lkpi_alloc_workqueue("lkpi_events_freezable",
	    WQ_FREEZABLE, 2);
	system_power_efficient_wq = lkpi_alloc_workqueue("lkpi_events_pwr",
	    WQ_POWER_EFFICIENT, 2);
	system_freezable_power_efficient_wq = lkpi_alloc_workqueue(
	    "lkpi_events_frz_pwr", WQ_FREEZABLE | WQ_POWER_EFFICIENT, 2);
	lkpi_irq_work_wq = lkpi_alloc_workqueue("lkpi_irq_work", WQ_HIGHPRI,
	    4);
	lkpi_tasklet_wq = lkpi_alloc_workqueue("lkpi_tasklet", WQ_HIGHPRI, 4);
	/* Linux's new names for the per-CPU and the unbound queue. */
	system_percpu_wq = system_wq;
	system_dfl_wq = system_unbound_wq;

	if (system_wq == NULL || system_highpri_wq == NULL ||
	    system_long_wq == NULL || system_unbound_wq == NULL ||
	    system_freezable_wq == NULL || system_power_efficient_wq == NULL ||
	    system_freezable_power_efficient_wq == NULL ||
	    lkpi_irq_work_wq == NULL || lkpi_tasklet_wq == NULL) {
		lkpi_work_fini();
		return (-ENOMEM);
	}
	return (0);
}

static void
lkpi_wq_release(struct workqueue_struct **wqp)
{
	struct workqueue_struct *wq = *wqp;

	*wqp = NULL;
	destroy_workqueue(wq);
}

void
lkpi_work_fini(void)
{
	/* RCU callbacks may still queue work (queue_rcu_work()). */
	rcu_barrier();

	system_percpu_wq = NULL;
	system_dfl_wq = NULL;
	lkpi_wq_release(&system_wq);
	lkpi_wq_release(&system_highpri_wq);
	lkpi_wq_release(&system_long_wq);
	lkpi_wq_release(&system_unbound_wq);
	lkpi_wq_release(&system_freezable_wq);
	lkpi_wq_release(&system_power_efficient_wq);
	lkpi_wq_release(&system_freezable_power_efficient_wq);
	lkpi_wq_release(&lkpi_tasklet_wq);
	lkpi_wq_release(&lkpi_irq_work_wq);

	cv_destroy(&lkpi_wq_done_cv);
	(mutex_destroy)(&lkpi_wq_lock);
}
