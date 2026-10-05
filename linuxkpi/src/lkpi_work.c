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
 * Workers wait on their queue's condition variable while it has nothing
 * pending, and on lkpi_wq_done_cv while everything pending runs on other
 * workers; queueing wakes both kinds, so a queue never sleeps with a
 * runnable item. A queue has max_active workers (0: the default), at
 * most LKPI_WQ_MAX_THREADS; both grow with the CPU count, so blocking
 * items cannot exhaust a pool as easily as a fixed small one.
 *
 * Delayed work arms the item's layer timer (src/lkpi_timer.c), whose
 * function queues the item if it is still the armed instance. Each queue
 * lists its armed delayed items; destroy_workqueue() disarms them.
 *
 * disable_work() counts disables in the item's data (as Linux does):
 * while the count is non-zero the item cannot be queued.
 *
 * lkpi_work_quiesce() removes the items whose function lies in an
 * unloading driver's text (lkpi_module_quiesce(), src/lkpi_module.c).
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
#define	LKPI_WORK_DIS_SHIFT	8		/* disable_work() count */
#define	LKPI_WORK_DIS_ONE	(1UL << LKPI_WORK_DIS_SHIFT)
#define	LKPI_WORK_DIS_MASK	(0xffffUL << LKPI_WORK_DIS_SHIFT)
#define	LKPI_WORK_GEN_SHIFT	24
#define	LKPI_WORK_GEN_ONE	(1UL << LKPI_WORK_GEN_SHIFT)

/* Worker threads per queue: the default and the limit. */
#define	LKPI_WQ_DFL_THREADS	MAX(8, ncpus)
#define	LKPI_WQ_MAX_THREADS	MIN(MAX(16, 2 * ncpus), 256)

struct workqueue_struct {
	char name[32];
	unsigned int flags;
	int nthreads;
	ddi_taskq_t *tq;
	struct list_head link;		/* lkpi_wq_all */
	struct list_head pending;
	struct list_head delayed;	/* armed delayed_work.lkpi_dlink */
	kcondvar_t cv;			/* workers: new or runnable items */
	unsigned int nwait_done;	/* workers waiting on lkpi_wq_done_cv */
	unsigned int color;		/* colour of newly queued items */
	unsigned long inflight[2];	/* pending or running, per colour */
	kmutex_t flush_lock;		/* serialises flush_workqueue() */
	boolean_t exiting;
};

/* A worker thread executing an item (on the worker's stack). */
struct lkpi_worker {
	struct list_head link;
	struct workqueue_struct *wq;
	struct work_struct *cur;
	uintptr_t cb;			/* the driver function behind cur */
	kthread_t *thread;
};

static kmutex_t lkpi_wq_lock;
static kcondvar_t lkpi_wq_done_cv;	/* an item ran or was cancelled */
static struct list_head lkpi_wq_running = LIST_HEAD_INIT(lkpi_wq_running);
static struct list_head lkpi_wq_all = LIST_HEAD_INIT(lkpi_wq_all);

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

/* Whether d (an item's data) refuses queueing. */
static inline boolean_t
wd_blocked(unsigned long d)
{
	return ((d & (LKPI_WORK_PENDING | LKPI_WORK_CANCELING |
	    LKPI_WORK_DIS_MASK)) != 0);
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
	/* Workers that skipped items running elsewhere can run this one. */
	if (wq->nwait_done != 0)
		cv_broadcast(&lkpi_wq_done_cv);
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
	if (wd_blocked(wd_get(w)))
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
		list_del_init(&dw->lkpi_dlink);
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

static void lkpi_tasklet_work(struct work_struct *w);
static void lkpi_irq_work_fn(struct work_struct *w);

/*
 * The driver function an item runs: the item's own, or for the layer's
 * tasklet and irq_work items the tasklet's or irq_work's function.
 */
static uintptr_t
lkpi_work_callback(struct work_struct *w)
{
	struct tasklet_struct *t;

	if (w->func == lkpi_tasklet_work) {
		t = container_of(w, struct tasklet_struct, lkpi_work);
		return (t->use_callback ? (uintptr_t)t->callback :
		    (uintptr_t)t->func);
	}
	if (w->func == lkpi_irq_work_fn)
		return ((uintptr_t)container_of(w, struct irq_work,
		    work)->func);
	return ((uintptr_t)w->func);
}

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
	me.cb = 0;
	me.thread = curthread;
	mutex_enter(&lkpi_wq_lock);
	for (;;) {
		w = lkpi_wq_next_locked(wq);
		if (w == NULL) {
			if (wq->exiting && list_empty(&wq->pending))
				break;
			if (list_empty(&wq->pending)) {
				cv_wait(&wq->cv, &lkpi_wq_lock);
				continue;
			}
			/* Items running elsewhere: wait for a run to end. */
			wq->nwait_done++;
			cv_wait(&lkpi_wq_done_cv, &lkpi_wq_lock);
			wq->nwait_done--;
			continue;
		}
		list_del_init(&w->entry);
		d = wd_get(w);
		color = (d & LKPI_WORK_COLOR) != 0;
		wd_set(w, (d & ~LKPI_WORK_PENDING) + LKPI_WORK_GEN_ONE);
		func = w->func;
		me.cur = w;
		me.cb = lkpi_work_callback(w);
		list_add(&me.link, &lkpi_wq_running);
		mutex_exit(&lkpi_wq_lock);

		if (func != NULL)
			func(w);
		else
			WARN_ONCE(1, "linuxkpi: work item without a function");

		mutex_enter(&lkpi_wq_lock);
		list_del(&me.link);
		me.cur = NULL;
		me.cb = 0;
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
	INIT_LIST_HEAD(&wq->delayed);
	INIT_LIST_HEAD(&wq->link);
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
	mutex_enter(&lkpi_wq_lock);
	list_add_tail(&wq->link, &lkpi_wq_all);
	mutex_exit(&lkpi_wq_lock);
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

/*
 * Delayed items still armed for the queue would queue themselves to the
 * freed queue when their timers fire: they are disarmed (and reported, as
 * Linux leaves them to fire into freed memory). Their timer functions may
 * be running already; those find the items disarmed and only read the
 * items, which belong to the caller.
 */
void
destroy_workqueue(struct workqueue_struct *wq)
{
	struct delayed_work *dw;
	unsigned long n = 0;

	if (wq == NULL)
		return;
	drain_workqueue(wq);
	mutex_enter(&lkpi_wq_lock);
	while (!list_empty(&wq->delayed)) {
		dw = list_first_entry(&wq->delayed, struct delayed_work,
		    lkpi_dlink);
		/* Armed items are pending, so this always takes dw off. */
		if (!lkpi_work_grab_locked(&dw->work))
			list_del_init(&dw->lkpi_dlink);
		n++;
	}
	if (n != 0)
		cmn_err(CE_WARN, "linuxkpi: workqueue %s destroyed with %lu "
		    "delayed items armed, disarmed", wq->name, n);
	wq->exiting = B_TRUE;
	list_del_init(&wq->link);
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
		list_del_init(&dw->lkpi_dlink);
		wq = dw->wq;
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
	INIT_LIST_HEAD(&dw->lkpi_dlink);
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
	list_add_tail(&dw->lkpi_dlink, &wq->delayed);
	(void) mod_timer(&dw->timer, lkpi_jiffies() + delay);
}

bool
queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
    unsigned long delay)
{
	bool queued = false;

	mutex_enter(&lkpi_wq_lock);
	if (!wd_blocked(wd_get(&dw->work)) && lkpi_wq_usable(wq)) {
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
		/* A disabled item is only cancelled, as on Linux. */
		if ((wd_get(&dw->work) & LKPI_WORK_DIS_MASK) == 0)
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
		list_del_init(&dw->lkpi_dlink);
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

/* --- disabling ------------------------------------------------------------------ */

/* Counts a disable of w; false (and a warning) if the count is full. */
static boolean_t
lkpi_work_disable_locked(struct work_struct *w)
{
	if ((wd_get(w) & LKPI_WORK_DIS_MASK) == LKPI_WORK_DIS_MASK) {
		WARN_ONCE(1, "linuxkpi: work item disabled too often");
		return (B_FALSE);
	}
	wd_set(w, wd_get(w) + LKPI_WORK_DIS_ONE);
	return (B_TRUE);
}

/* Disables w and cancels it; whether it was pending. */
bool
disable_work(struct work_struct *w)
{
	bool was = false;

	mutex_enter(&lkpi_wq_lock);
	if (lkpi_work_disable_locked(w))
		was = lkpi_work_grab_locked(w);
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

/* disable_work(), then waits until w does not run. */
bool
disable_work_sync(struct work_struct *w)
{
	bool was = false;

	if (!lkpi_work_may_wait())
		return (disable_work(w));
	mutex_enter(&lkpi_wq_lock);
	if (lkpi_work_disable_locked(w)) {
		was = lkpi_work_grab_locked(w);
		lkpi_work_wait_idle_locked(w);
	}
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

/* Undoes one disable; true when w can be queued again. */
bool
enable_work(struct work_struct *w)
{
	bool enabled;

	mutex_enter(&lkpi_wq_lock);
	if ((wd_get(w) & LKPI_WORK_DIS_MASK) == 0) {
		WARN_ONCE(1, "linuxkpi: enable_work() of an enabled item");
		enabled = true;
	} else {
		wd_set(w, wd_get(w) - LKPI_WORK_DIS_ONE);
		enabled = (wd_get(w) & LKPI_WORK_DIS_MASK) == 0;
	}
	mutex_exit(&lkpi_wq_lock);
	return (enabled);
}

bool
disable_delayed_work(struct delayed_work *dw)
{
	boolean_t delayed;
	bool was = false;

	mutex_enter(&lkpi_wq_lock);
	delayed = (wd_get(&dw->work) & LKPI_WORK_DELAYED) != 0;
	if (lkpi_work_disable_locked(&dw->work))
		was = lkpi_work_grab_locked(&dw->work);
	mutex_exit(&lkpi_wq_lock);
	if (delayed && !servicing_interrupt())
		lkpi_timer_wait_idle(&dw->timer);
	return (was);
}

bool
disable_delayed_work_sync(struct delayed_work *dw)
{
	bool was;

	if (!lkpi_work_may_wait())
		return (disable_delayed_work(dw));
	was = disable_delayed_work(dw);
	mutex_enter(&lkpi_wq_lock);
	lkpi_work_wait_idle_locked(&dw->work);
	mutex_exit(&lkpi_wq_lock);
	return (was);
}

bool
enable_delayed_work(struct delayed_work *dw)
{
	return (enable_work(&dw->work));
}

/* --- RCU work ---------------------------------------------------------------- */

static void
lkpi_rcu_work_rcufn(struct rcu_head *head)
{
	struct rcu_work *rw = container_of(head, struct rcu_work, rcu);

	mutex_enter(&lkpi_wq_lock);
	wd_clear(&rw->work, LKPI_WORK_RCU);
	if ((wd_get(&rw->work) & LKPI_WORK_DIS_MASK) == 0 &&
	    lkpi_wq_usable(rw->wq)) {
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
	if (wd_blocked(wd_get(&rw->work)) || !lkpi_wq_usable(wq)) {
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

/*
 * Tasklets run on a layer thread that an interrupt handler may have
 * interrupted on this CPU, so in an interrupt the spin is bounded and
 * then reported (the caller proceeds as if the tasklet had finished).
 */
#define	LKPI_TASKLET_SPIN_NS	(10LL * NANOSEC / MILLISEC)

void
tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
	hrtime_t start;

	if (!servicing_interrupt()) {
		while ((ts_get(t) & LKPI_TASKLET_RUN) != 0)
			cpu_relax();
		return;
	}
	start = gethrtime();
	while ((ts_get(t) & LKPI_TASKLET_RUN) != 0) {
		if (gethrtime() - start > LKPI_TASKLET_SPIN_NS) {
			WARN_ONCE(1, "linuxkpi: tasklet %p still running "
			    "after 10 ms of waiting in an interrupt",
			    (void *)t);
			return;
		}
		cpu_relax();
	}
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

/* --- driver unload ------------------------------------------------------------ */

/* Flushes the layer's shared queues (lkpi_module_quiesce()). */
void
lkpi_work_flush_system(void)
{
	flush_workqueue(system_wq);
	flush_workqueue(system_highpri_wq);
	flush_workqueue(system_long_wq);
	flush_workqueue(system_unbound_wq);
	flush_workqueue(system_freezable_wq);
	flush_workqueue(system_power_efficient_wq);
	flush_workqueue(system_freezable_power_efficient_wq);
	flush_workqueue(lkpi_irq_work_wq);
	flush_workqueue(lkpi_tasklet_wq);
}

static boolean_t
lkpi_work_in(uintptr_t cb, uintptr_t lo, uintptr_t hi)
{
	return (cb >= lo && cb < hi);
}

/*
 * The first armed delayed item or pending item, on any queue, whose
 * function lies in [lo, hi).
 */
static struct work_struct *
lkpi_work_find_locked(uintptr_t lo, uintptr_t hi)
{
	struct workqueue_struct *wq;
	struct delayed_work *dw;
	struct work_struct *w;

	list_for_each_entry(wq, &lkpi_wq_all, link) {
		list_for_each_entry(dw, &wq->delayed, lkpi_dlink) {
			if (lkpi_work_in(lkpi_work_callback(&dw->work), lo, hi))
				return (&dw->work);
		}
		list_for_each_entry(w, &wq->pending, entry) {
			if (lkpi_work_in(lkpi_work_callback(w), lo, hi))
				return (w);
		}
	}
	return (NULL);
}

static boolean_t
lkpi_work_running_in_locked(uintptr_t lo, uintptr_t hi)
{
	struct lkpi_worker *wk;

	list_for_each_entry(wk, &lkpi_wq_running, link) {
		if (lkpi_work_in(wk->cb, lo, hi))
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Cancels every armed delayed item and pending item whose function lies
 * in [lo, hi), the text of a driver that is unloading, and waits until no
 * such function runs. Each one is a driver bug (module_exit should have
 * cancelled it), so it is reported. Returns how many were cancelled.
 */
int
lkpi_work_quiesce(uintptr_t lo, uintptr_t hi)
{
	struct work_struct *w;
	struct delayed_work *dw;
	boolean_t delayed;
	int n = 0;

	if (!lkpi_work_may_wait())
		return (0);
	mutex_enter(&lkpi_wq_lock);
	for (;;) {
		w = lkpi_work_find_locked(lo, hi);
		if (w == NULL) {
			/* A running item may queue itself again. */
			if (!lkpi_work_running_in_locked(lo, hi))
				break;
			cv_wait(&lkpi_wq_done_cv, &lkpi_wq_lock);
			continue;
		}
		delayed = (wd_get(w) & LKPI_WORK_DELAYED) != 0;
		cmn_err(CE_WARN, "linuxkpi: work item %p (function %p) still "
		    "%s at driver unload, cancelled", (void *)w,
		    (void *)lkpi_work_callback(w),
		    delayed ? "armed" : "queued");
		if (!lkpi_work_grab_locked(w)) {
			/* Not reached: listed items are pending. */
			WARN_ONCE(1, "linuxkpi: work item %p not cancellable",
			    (void *)w);
			break;
		}
		n++;
		if (delayed) {
			dw = to_delayed_work(w);
			mutex_exit(&lkpi_wq_lock);
			lkpi_timer_wait_idle(&dw->timer);
			mutex_enter(&lkpi_wq_lock);
		}
	}
	mutex_exit(&lkpi_wq_lock);
	return (n);
}

/* --- module state ------------------------------------------------------------- */

int
lkpi_work_init(void)
{
	(mutex_init)(&lkpi_wq_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&lkpi_wq_done_cv, NULL, CV_DRIVER, NULL);
	INIT_LIST_HEAD(&lkpi_wq_running);

	INIT_LIST_HEAD(&lkpi_wq_all);

	/* The general queues get the default, CPU-scaled worker count. */
	system_wq = lkpi_alloc_workqueue("lkpi_events", 0, 0);
	system_highpri_wq = lkpi_alloc_workqueue("lkpi_events_highpri",
	    WQ_HIGHPRI, 4);
	system_long_wq = lkpi_alloc_workqueue("lkpi_events_long", 0, 0);
	system_unbound_wq = lkpi_alloc_workqueue("lkpi_events_unbound",
	    WQ_UNBOUND, 0);
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
