/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's RCU and sleepable RCU (<linux/rcupdate.h>, <linux/srcu.h>).
 *
 * Grace periods (RCU and each srcu_struct): two reader counts and a phase
 * index. A reader increments the count of the current phase (followed by
 * a full barrier) and decrements the same count when it leaves. A grace
 * period, serialised by gp_lock:
 *	1. waits until the count of the other phase is zero;
 *	2. flips the phase;
 *	3. waits until the count of the old phase is zero.
 * Readers that start after the flip use the new phase, so step 3 waits
 * only for readers that overlap the grace period. A reader that read the
 * old phase before the flip but counted itself only after step 3 found
 * the count at zero sees the updater's data (the updater's store, its
 * barrier and its read of the count against the reader's increment, its
 * barrier and its reads); it may still be reading when the next grace
 * period starts, which is why step 1 of that period waits for it. Both
 * waits are bounded by read sections that already began.
 *
 * Waiters sleep on the structure's condition variable; the reader that
 * brings a count to zero while someone waits broadcasts it, and a wake-up
 * lost between the waiter's test and its sleep is caught by re-testing
 * every clock tick. After 10 seconds a wait reports that it is still
 * waiting (a grace period started inside a read section waits for
 * itself).
 *
 * RCU readers remember the phase of their outermost read section: in the
 * task of the thread (`current`, <linux/sched.h>) in thread context; in
 * an interrupt handler, where `current` is a shared placeholder, in a
 * slot of lkpi_rcu_slots claimed by the interrupt thread, found by
 * hashing curthread and probing a few slots. Read sections nest. Either
 * record belongs to one thread, so a reader that moves to another CPU
 * keeps it. High-level interrupt handlers (which run on the interrupted
 * thread's stack) are not supported; the layer never installs them.
 * RCU grace periods are numbered (rcu_gp_seq: odd while one runs) for
 * the polled interface.
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
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/* RCU's grace-period state: the same machinery as an srcu_struct. */
static struct srcu_struct lkpi_rcu_gp;
static kmutex_t lkpi_rcu_sync_lock;	/* a numbered grace period */
static volatile unsigned long lkpi_rcu_gp_seq;

/* The phase records of interrupt-context RCU readers. */
#define	LKPI_RCU_SLOTS	256
#define	LKPI_RCU_PROBE	16

struct lkpi_rcu_slot {
	kthread_t *volatile owner;
	unsigned int nest;
	unsigned int idx;
};

static struct lkpi_rcu_slot lkpi_rcu_slots[LKPI_RCU_SLOTS];

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

/* --- grace-period machinery (RCU and SRCU) ------------------------------ */

/* Counts a reader in the current phase; returns the phase. */
static unsigned int
lkpi_gp_enter(struct srcu_struct *gp)
{
	unsigned int idx = __atomic_load_n(&gp->idx, __ATOMIC_SEQ_CST) & 1;

	(void) __atomic_add_fetch(&gp->readers[idx], 1, __ATOMIC_SEQ_CST);
	/* The read section's loads follow the increment. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return (idx);
}

/* Uncounts a reader of phase idx; wakes a waiting grace period. */
static void
lkpi_gp_exit(struct srcu_struct *gp, unsigned int idx)
{
	if (__atomic_sub_fetch(&gp->readers[idx], 1, __ATOMIC_SEQ_CST) != 0 ||
	    __atomic_load_n(&gp->waiters, __ATOMIC_SEQ_CST) == 0)
		return;
	mutex_enter(&gp->lock);
	cv_broadcast(&gp->cv);
	mutex_exit(&gp->lock);
}

/* Waits until phase idx has no readers. */
static void
lkpi_gp_wait_phase(struct srcu_struct *gp, unsigned int idx,
    const char *what, hrtime_t start, boolean_t *warned)
{
	if (__atomic_load_n(&gp->readers[idx], __ATOMIC_SEQ_CST) == 0)
		return;
	(void) __atomic_add_fetch(&gp->waiters, 1, __ATOMIC_SEQ_CST);
	mutex_enter(&gp->lock);
	while (__atomic_load_n(&gp->readers[idx], __ATOMIC_SEQ_CST) != 0) {
		(void) cv_reltimedwait(&gp->cv, &gp->lock, 1, TR_CLOCK_TICK);
		if (!*warned && gethrtime() - start > LKPI_RCU_WARN_NS) {
			*warned = B_TRUE;
			cmn_err(CE_WARN, "linuxkpi: %s has waited 10 s for "
			    "readers (called inside a read section?)", what);
		}
	}
	mutex_exit(&gp->lock);
	(void) __atomic_sub_fetch(&gp->waiters, 1, __ATOMIC_SEQ_CST);
}

/* A full grace period of gp (see the top of the file). */
static void
lkpi_gp_sync(struct srcu_struct *gp, const char *what)
{
	hrtime_t start = gethrtime();
	boolean_t warned = B_FALSE;
	unsigned int idx;

	/* The caller's updates before the reader counts are read. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	mutex_enter(&gp->gp_lock);
	idx = __atomic_load_n(&gp->idx, __ATOMIC_SEQ_CST) & 1;
	lkpi_gp_wait_phase(gp, idx ^ 1, what, start, &warned);
	__atomic_store_n(&gp->idx, idx ^ 1, __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	lkpi_gp_wait_phase(gp, idx, what, start, &warned);
	mutex_exit(&gp->gp_lock);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void
lkpi_gp_init(struct srcu_struct *gp)
{
	gp->readers[0] = gp->readers[1] = 0;
	gp->idx = 0;
	gp->waiters = 0;
	(mutex_init)(&gp->lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&gp->cv, NULL, CV_DRIVER, NULL);
	(mutex_init)(&gp->gp_lock, NULL, MUTEX_DRIVER, NULL);
}

static void
lkpi_gp_fini(struct srcu_struct *gp)
{
	WARN_ON(gp->readers[0] != 0 || gp->readers[1] != 0);
	(mutex_destroy)(&gp->gp_lock);
	cv_destroy(&gp->cv);
	(mutex_destroy)(&gp->lock);
}

/* --- RCU readers -------------------------------------------------------- */

static inline uint_t
lkpi_rcu_hash(const kthread_t *t)
{
	uintptr_t h = (uintptr_t)t;

	return ((uint_t)((h >> 6) ^ (h >> 14)) % LKPI_RCU_SLOTS);
}

/* The slot of the calling interrupt thread, or NULL if it has none. */
static struct lkpi_rcu_slot *
lkpi_rcu_slot_find(void)
{
	uint_t h = lkpi_rcu_hash(curthread), i;
	struct lkpi_rcu_slot *sl;

	for (i = 0; i < LKPI_RCU_PROBE; i++) {
		sl = &lkpi_rcu_slots[(h + i) % LKPI_RCU_SLOTS];
		if (sl->owner == curthread)
			return (sl);
	}
	return (NULL);
}

/*
 * Claims a free slot for the calling interrupt thread. All slots of its
 * probe range are taken only with dozens of nested interrupt readers;
 * spin until one leaves.
 */
static struct lkpi_rcu_slot *
lkpi_rcu_slot_claim(void)
{
	uint_t h = lkpi_rcu_hash(curthread), i;
	struct lkpi_rcu_slot *sl;
	kthread_t *expect;

	for (;;) {
		for (i = 0; i < LKPI_RCU_PROBE; i++) {
			sl = &lkpi_rcu_slots[(h + i) % LKPI_RCU_SLOTS];
			expect = NULL;
			if (sl->owner == NULL &&
			    __atomic_compare_exchange_n(&sl->owner, &expect,
			    curthread, B_FALSE, __ATOMIC_SEQ_CST,
			    __ATOMIC_SEQ_CST)) {
				sl->nest = 0;
				return (sl);
			}
		}
		WARN_ONCE(1, "linuxkpi: RCU reader slots exhausted");
		cpu_relax();
	}
}

void
rcu_read_lock(void)
{
	struct task_struct *t = lkpi_task_self();
	struct lkpi_rcu_slot *sl;

	if (t != NULL) {
		if (t->lkpi_rcu_nest++ == 0)
			t->lkpi_rcu_idx = lkpi_gp_enter(&lkpi_rcu_gp);
	} else {
		sl = lkpi_rcu_slot_find();
		if (sl == NULL)
			sl = lkpi_rcu_slot_claim();
		if (sl->nest++ == 0)
			sl->idx = lkpi_gp_enter(&lkpi_rcu_gp);
	}
	barrier();
}

void
rcu_read_unlock(void)
{
	struct task_struct *t = lkpi_task_self();
	struct lkpi_rcu_slot *sl;
	unsigned int idx;

	barrier();
	if (t != NULL) {
		if (t->lkpi_rcu_nest == 0) {
			WARN_ONCE(1, "linuxkpi: rcu_read_unlock() without "
			    "rcu_read_lock()");
			return;
		}
		if (--t->lkpi_rcu_nest == 0)
			lkpi_gp_exit(&lkpi_rcu_gp, t->lkpi_rcu_idx);
		return;
	}
	sl = lkpi_rcu_slot_find();
	if (sl == NULL || sl->nest == 0) {
		WARN_ONCE(1, "linuxkpi: rcu_read_unlock() without "
		    "rcu_read_lock() in an interrupt");
		return;
	}
	if (--sl->nest == 0) {
		idx = sl->idx;
		__atomic_store_n(&sl->owner, NULL, __ATOMIC_SEQ_CST);
		lkpi_gp_exit(&lkpi_rcu_gp, idx);
	}
}

void
synchronize_rcu(void)
{
	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: synchronize_rcu() in an interrupt");
		return;
	}
	mutex_enter(&lkpi_rcu_sync_lock);
	(void) __atomic_add_fetch(&lkpi_rcu_gp_seq, 1, __ATOMIC_SEQ_CST);
	lkpi_gp_sync(&lkpi_rcu_gp, "synchronize_rcu()");
	(void) __atomic_add_fetch(&lkpi_rcu_gp_seq, 1, __ATOMIC_SEQ_CST);
	mutex_exit(&lkpi_rcu_sync_lock);
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
	lkpi_gp_init(ssp);
	return (0);
}

void
cleanup_srcu_struct(struct srcu_struct *ssp)
{
	lkpi_gp_fini(ssp);
}

int
srcu_read_lock(struct srcu_struct *ssp)
{
	return ((int)lkpi_gp_enter(ssp));
}

void
srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	if (idx != 0 && idx != 1) {
		WARN_ONCE(1, "linuxkpi: srcu_read_unlock() with index %d", idx);
		return;
	}
	if (__atomic_load_n(&ssp->readers[idx], __ATOMIC_SEQ_CST) == 0) {
		WARN_ONCE(1, "linuxkpi: srcu_read_unlock() without a reader");
		return;
	}
	lkpi_gp_exit(ssp, (unsigned int)idx);
}

void
synchronize_srcu(struct srcu_struct *ssp)
{
	might_sleep();
	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: synchronize_srcu() in an interrupt");
		return;
	}
	lkpi_gp_sync(ssp, "synchronize_srcu()");
}

/* --- module state ---------------------------------------------------------- */

int
lkpi_rcu_init(void)
{
	lkpi_gp_init(&lkpi_rcu_gp);
	(mutex_init)(&lkpi_rcu_sync_lock, NULL, MUTEX_DRIVER, NULL);
	bzero(lkpi_rcu_slots, sizeof (lkpi_rcu_slots));
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
	(mutex_destroy)(&lkpi_rcu_sync_lock);
	lkpi_gp_fini(&lkpi_rcu_gp);
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

	cv_destroy(&lkpi_rcu_done_cv);
	cv_destroy(&lkpi_rcu_cb_cv);
	(mutex_destroy)(&lkpi_rcu_cb_lock);
	(mutex_destroy)(&lkpi_rcu_sync_lock);
	lkpi_gp_fini(&lkpi_rcu_gp);
}
