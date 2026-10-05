/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's RCU and sleepable RCU (<linux/rcupdate.h>, <linux/srcu.h>).
 *
 * Grace periods: rcu_read_lock() and rcu_read_unlock() count the readers
 * in lkpi_rcu_readers (inline, sequentially consistent atomics).
 * synchronize_rcu() returns once the count was zero at some instant after
 * its start: every reader that was inside a read section when it started
 * has then left it. The writer cannot poll for that instant (readers on
 * other processors may keep the count above zero most of the time), so
 * while a writer waits, the reader that brings the count to zero bumps an
 * epoch and wakes it (lkpi_rcu_read_unlock_slow()); a wake-up missed in
 * the window between the writer's tests is caught by re-testing every
 * clock tick. Grace periods are serialised and numbered (rcu_gp_seq: odd
 * while one runs) for the polled interface.
 *
 * synchronize_rcu() must not be called inside a read section (it would
 * wait for itself); after 10 seconds it reports that it is still waiting.
 *
 * Callbacks (call_rcu(), kfree_rcu()) are queued in order for one layer
 * thread (a single-thread ddi_taskq(9F)) that takes the whole queue,
 * waits for a grace period and runs them in thread context. kfree_rcu()
 * needs no callback function: the queued head records the offset of the
 * head in the object (a value no kernel function address can have), and
 * the thread frees the object with kvfree(). rcu_barrier() waits until
 * every callback queued before it has run.
 */

#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

volatile uint32_t lkpi_rcu_readers;

/* Writers waiting for the reader count to reach zero, and its epoch. */
static volatile uint32_t lkpi_rcu_waiters;
static volatile uint32_t lkpi_rcu_epoch;
static kmutex_t lkpi_rcu_lock;		/* the epoch's sleepers */
static kcondvar_t lkpi_rcu_cv;
static kmutex_t lkpi_rcu_gp_lock;	/* serialises grace periods */
static volatile unsigned long lkpi_rcu_gp_seq;

/* The callback queue and its thread. */
static kmutex_t lkpi_rcu_cb_lock;
static kcondvar_t lkpi_rcu_cb_cv;	/* work for the thread */
static kcondvar_t lkpi_rcu_done_cv;	/* callbacks have run */
static struct rcu_head *lkpi_rcu_cb_head;
static struct rcu_head **lkpi_rcu_cb_tail = &lkpi_rcu_cb_head;
static uint64_t lkpi_rcu_cb_queued;	/* callbacks ever queued */
static uint64_t lkpi_rcu_cb_done;	/* callbacks ever run */
static boolean_t lkpi_rcu_gp_wanted;	/* a poller waits for a grace period */
static boolean_t lkpi_rcu_exiting;
static ddi_taskq_t *lkpi_rcu_tq;

/* A kvfree_rcu() head: the head's offset in its object, plus one. */
#define	LKPI_RCU_KVFREE_LIMIT	(1UL << 32)
#define	LKPI_RCU_IS_KVFREE(f)	((uintptr_t)(f) - 1 < LKPI_RCU_KVFREE_LIMIT)

#define	LKPI_RCU_WARN_NS	(10LL * NANOSEC)

/*
 * Waits until *readers was zero at some instant after the call, given
 * that a reader bringing it to zero while *waiters is non-zero
 * increments *epoch and broadcasts cv under lock. Called with lock held.
 */
static void
lkpi_rcu_wait_readers(volatile uint32_t *readers, volatile uint32_t *waiters,
    volatile uint32_t *epoch, kmutex_t *lock, kcondvar_t *cv,
    const char *what)
{
	hrtime_t start = gethrtime();
	boolean_t warned = B_FALSE;
	uint32_t e0;

	(void) __atomic_add_fetch(waiters, 1, __ATOMIC_SEQ_CST);
	e0 = __atomic_load_n(epoch, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(readers, __ATOMIC_SEQ_CST) != 0 &&
	    __atomic_load_n(epoch, __ATOMIC_SEQ_CST) == e0) {
		(void) cv_reltimedwait(cv, lock, 1, TR_CLOCK_TICK);
		if (!warned && gethrtime() - start > LKPI_RCU_WARN_NS) {
			warned = B_TRUE;
			cmn_err(CE_WARN, "linuxkpi: %s has waited 10 s for "
			    "readers (called inside a read section?)", what);
		}
	}
	(void) __atomic_sub_fetch(waiters, 1, __ATOMIC_SEQ_CST);
}

void
lkpi_rcu_read_unlock_slow(void)
{
	if (__atomic_load_n(&lkpi_rcu_waiters, __ATOMIC_SEQ_CST) == 0)
		return;
	mutex_enter(&lkpi_rcu_lock);
	(void) __atomic_add_fetch(&lkpi_rcu_epoch, 1, __ATOMIC_SEQ_CST);
	cv_broadcast(&lkpi_rcu_cv);
	mutex_exit(&lkpi_rcu_lock);
}

void
synchronize_rcu(void)
{
	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: synchronize_rcu() in an interrupt");
		return;
	}
	/* The caller's updates before the reader count is read. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	mutex_enter(&lkpi_rcu_gp_lock);
	(void) __atomic_add_fetch(&lkpi_rcu_gp_seq, 1, __ATOMIC_SEQ_CST);
	mutex_enter(&lkpi_rcu_lock);
	lkpi_rcu_wait_readers(&lkpi_rcu_readers, &lkpi_rcu_waiters,
	    &lkpi_rcu_epoch, &lkpi_rcu_lock, &lkpi_rcu_cv, "synchronize_rcu()");
	mutex_exit(&lkpi_rcu_lock);
	(void) __atomic_add_fetch(&lkpi_rcu_gp_seq, 1, __ATOMIC_SEQ_CST);
	mutex_exit(&lkpi_rcu_gp_lock);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/*
 * The grace-period number that completes once a full grace period has
 * started after the call: the next even number after the current
 * period, if one runs.
 */
unsigned long
get_state_synchronize_rcu(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return ((__atomic_load_n(&lkpi_rcu_gp_seq, __ATOMIC_SEQ_CST) + 3) &
	    ~1UL);
}

static boolean_t
lkpi_rcu_gp_done(unsigned long state)
{
	boolean_t done = (long)(__atomic_load_n(&lkpi_rcu_gp_seq,
	    __ATOMIC_SEQ_CST) - state) >= 0;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return (done);
}

/*
 * Grace periods run only when someone waits for one, so a poller that
 * finds its period incomplete asks the callback thread to run one.
 */
bool
poll_state_synchronize_rcu(unsigned long oldstate)
{
	if (lkpi_rcu_gp_done(oldstate))
		return (true);
	mutex_enter(&lkpi_rcu_cb_lock);
	if (lkpi_rcu_tq != NULL && !lkpi_rcu_gp_wanted) {
		lkpi_rcu_gp_wanted = B_TRUE;
		cv_signal(&lkpi_rcu_cb_cv);
	}
	mutex_exit(&lkpi_rcu_cb_lock);
	return (false);
}

void
cond_synchronize_rcu(unsigned long oldstate)
{
	if (!lkpi_rcu_gp_done(oldstate))
		synchronize_rcu();
}

/* --- callbacks ---------------------------------------------------------- */

static void
lkpi_rcu_invoke(struct rcu_head *head)
{
	rcu_callback_t func = head->func;

	if (LKPI_RCU_IS_KVFREE(func))
		kvfree((char *)head - ((uintptr_t)func - 1));
	else
		func(head);
}

static void
lkpi_rcu_enqueue(struct rcu_head *head)
{
	mutex_enter(&lkpi_rcu_cb_lock);
	if (lkpi_rcu_tq != NULL && !lkpi_rcu_exiting) {
		head->next = NULL;
		*lkpi_rcu_cb_tail = head;
		lkpi_rcu_cb_tail = &head->next;
		lkpi_rcu_cb_queued++;
		cv_signal(&lkpi_rcu_cb_cv);
		mutex_exit(&lkpi_rcu_cb_lock);
		return;
	}
	mutex_exit(&lkpi_rcu_cb_lock);

	/* Outside the module's lifetime: run it here if this may sleep. */
	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: RCU callback dropped (no RCU thread)");
		return;
	}
	synchronize_rcu();
	lkpi_rcu_invoke(head);
}

void
call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	if (WARN_ON(func == NULL || LKPI_RCU_IS_KVFREE(func)))
		return;
	head->func = func;
	lkpi_rcu_enqueue(head);
}

/* kfree_rcu(ptr, field): kvfree(ptr) after a grace period. */
void
lkpi_kvfree_rcu(void *ptr, struct rcu_head *head)
{
	uintptr_t off;

	if (ptr == NULL)
		return;
	off = (uintptr_t)head - (uintptr_t)ptr;
	if (WARN_ON(off >= LKPI_RCU_KVFREE_LIMIT - 1))
		return;
	head->func = (rcu_callback_t)(off + 1);
	lkpi_rcu_enqueue(head);
}

void
rcu_barrier(void)
{
	uint64_t target;

	might_sleep();
	mutex_enter(&lkpi_rcu_cb_lock);
	target = lkpi_rcu_cb_queued;
	while (lkpi_rcu_cb_done < target && lkpi_rcu_tq != NULL)
		cv_wait(&lkpi_rcu_done_cv, &lkpi_rcu_cb_lock);
	mutex_exit(&lkpi_rcu_cb_lock);
}

/* The callback thread: until unload, run batches after a grace period. */
static void
lkpi_rcu_thread(void *arg)
{
	struct rcu_head *list, *head;
	uint64_t n;

	mutex_enter(&lkpi_rcu_cb_lock);
	for (;;) {
		while (lkpi_rcu_cb_head == NULL && !lkpi_rcu_gp_wanted &&
		    !lkpi_rcu_exiting)
			cv_wait(&lkpi_rcu_cb_cv, &lkpi_rcu_cb_lock);
		if (lkpi_rcu_cb_head == NULL && !lkpi_rcu_gp_wanted)
			break;
		list = lkpi_rcu_cb_head;
		lkpi_rcu_cb_head = NULL;
		lkpi_rcu_cb_tail = &lkpi_rcu_cb_head;
		lkpi_rcu_gp_wanted = B_FALSE;
		mutex_exit(&lkpi_rcu_cb_lock);

		synchronize_rcu();
		for (n = 0; list != NULL; n++) {
			head = list;
			list = head->next;
			lkpi_rcu_invoke(head);
		}

		mutex_enter(&lkpi_rcu_cb_lock);
		lkpi_rcu_cb_done += n;
		cv_broadcast(&lkpi_rcu_done_cv);
	}
	mutex_exit(&lkpi_rcu_cb_lock);
}

/* --- SRCU ------------------------------------------------------------------ */

int
init_srcu_struct(struct srcu_struct *ssp)
{
	ssp->readers = 0;
	ssp->waiters = 0;
	ssp->epoch = 0;
	(mutex_init)(&ssp->lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ssp->cv, NULL, CV_DRIVER, NULL);
	return (0);
}

void
cleanup_srcu_struct(struct srcu_struct *ssp)
{
	WARN_ON(ssp->readers != 0);
	cv_destroy(&ssp->cv);
	(mutex_destroy)(&ssp->lock);
}

int
srcu_read_lock(struct srcu_struct *ssp)
{
	(void) __atomic_add_fetch(&ssp->readers, 1, __ATOMIC_SEQ_CST);
	return (0);
}

void
srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	if (__atomic_sub_fetch(&ssp->readers, 1, __ATOMIC_SEQ_CST) != 0 ||
	    __atomic_load_n(&ssp->waiters, __ATOMIC_SEQ_CST) == 0)
		return;
	mutex_enter(&ssp->lock);
	(void) __atomic_add_fetch(&ssp->epoch, 1, __ATOMIC_SEQ_CST);
	cv_broadcast(&ssp->cv);
	mutex_exit(&ssp->lock);
}

void
synchronize_srcu(struct srcu_struct *ssp)
{
	might_sleep();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	mutex_enter(&ssp->lock);
	lkpi_rcu_wait_readers(&ssp->readers, &ssp->waiters, &ssp->epoch,
	    &ssp->lock, &ssp->cv, "synchronize_srcu()");
	mutex_exit(&ssp->lock);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* --- module state ---------------------------------------------------------- */

int
lkpi_rcu_init(void)
{
	(mutex_init)(&lkpi_rcu_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&lkpi_rcu_cv, NULL, CV_DRIVER, NULL);
	(mutex_init)(&lkpi_rcu_gp_lock, NULL, MUTEX_DRIVER, NULL);
	(mutex_init)(&lkpi_rcu_cb_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&lkpi_rcu_cb_cv, NULL, CV_DRIVER, NULL);
	cv_init(&lkpi_rcu_done_cv, NULL, CV_DRIVER, NULL);
	lkpi_rcu_cb_head = NULL;
	lkpi_rcu_cb_tail = &lkpi_rcu_cb_head;
	lkpi_rcu_exiting = B_FALSE;
	lkpi_rcu_gp_wanted = B_FALSE;

	lkpi_rcu_tq = ddi_taskq_create(NULL, "lkpi_rcu", 1, LKPI_PRI_HIGH, 0);
	if (lkpi_rcu_tq == NULL)
		goto fail;
	if (ddi_taskq_dispatch(lkpi_rcu_tq, lkpi_rcu_thread, NULL,
	    DDI_SLEEP) != DDI_SUCCESS) {
		ddi_taskq_destroy(lkpi_rcu_tq);
		lkpi_rcu_tq = NULL;
		goto fail;
	}
	return (0);
fail:
	cv_destroy(&lkpi_rcu_done_cv);
	cv_destroy(&lkpi_rcu_cb_cv);
	(mutex_destroy)(&lkpi_rcu_cb_lock);
	(mutex_destroy)(&lkpi_rcu_gp_lock);
	cv_destroy(&lkpi_rcu_cv);
	(mutex_destroy)(&lkpi_rcu_lock);
	return (-ENOMEM);
}

void
lkpi_rcu_fini(void)
{
	ddi_taskq_t *tq;

	/* The thread runs everything queued before it leaves. */
	mutex_enter(&lkpi_rcu_cb_lock);
	lkpi_rcu_exiting = B_TRUE;
	cv_broadcast(&lkpi_rcu_cb_cv);
	tq = lkpi_rcu_tq;
	mutex_exit(&lkpi_rcu_cb_lock);
	ddi_taskq_destroy(tq);

	mutex_enter(&lkpi_rcu_cb_lock);
	lkpi_rcu_tq = NULL;
	cv_broadcast(&lkpi_rcu_done_cv);
	mutex_exit(&lkpi_rcu_cb_lock);

	WARN_ON(lkpi_rcu_readers != 0);
	cv_destroy(&lkpi_rcu_done_cv);
	cv_destroy(&lkpi_rcu_cb_cv);
	(mutex_destroy)(&lkpi_rcu_cb_lock);
	(mutex_destroy)(&lkpi_rcu_gp_lock);
	cv_destroy(&lkpi_rcu_cv);
	(mutex_destroy)(&lkpi_rcu_lock);
}
