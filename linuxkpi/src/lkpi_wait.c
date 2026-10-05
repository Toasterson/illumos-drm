/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's wait queues, completions and the shared wait queues of
 * wait_var_event()/wait_on_bit() (<linux/wait.h>, <linux/completion.h>,
 * <linux/wait_bit.h>), on the layer's tasks (src/lkpi_sched.c).
 *
 * The layout and the wake-function protocol are Linux's: a wake-up walks
 * the entries under the queue's lock and calls each entry's function,
 * which decides whether the entry counts as woken; it stops after `nr`
 * woken exclusive entries (nr 0: none is exclusive enough to stop it).
 * Entries may remove themselves (autoremove_wake_function) during the
 * walk. The functions here never allocate; wake-ups work in interrupt
 * handlers.
 */

#include <linux/wait.h>
#include <linux/wait_bit.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/* --- wake functions -------------------------------------------------- */

int
default_wake_function(struct wait_queue_entry *e, unsigned int mode,
    int flags, void *key)
{
	return (wake_up_state(e->private, mode));
}

int
autoremove_wake_function(struct wait_queue_entry *e, unsigned int mode,
    int sync, void *key)
{
	int woken = default_wake_function(e, mode, sync, key);

	if (woken)
		list_del_init(&e->entry);
	return (woken);
}

int
woken_wake_function(struct wait_queue_entry *e, unsigned int mode,
    int sync, void *key)
{
	/* Pairs with the barrier in wait_woken(). */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	e->flags |= WQ_FLAG_WOKEN;
	return (default_wake_function(e, mode, sync, key));
}

/* --- queueing ---------------------------------------------------------- */

void
add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	__add_wait_queue(wq, e);
	spin_unlock(&wq->lock);
}

void
add_wait_queue_exclusive(struct wait_queue_head *wq,
    struct wait_queue_entry *e)
{
	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	__add_wait_queue_entry_tail(wq, e);
	spin_unlock(&wq->lock);
}

void
remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	spin_lock(&wq->lock);
	__remove_wait_queue(wq, e);
	spin_unlock(&wq->lock);
}

void
prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e,
    int state)
{
	e->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	if (list_empty(&e->entry))
		__add_wait_queue(wq, e);
	set_current_state(state);
	spin_unlock(&wq->lock);
}

/* Returns whether the queue was empty before e joined it. */
bool
prepare_to_wait_exclusive(struct wait_queue_head *wq,
    struct wait_queue_entry *e, int state)
{
	bool was_empty = false;

	e->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq->lock);
	if (list_empty(&e->entry)) {
		was_empty = list_empty(&wq->head);
		__add_wait_queue_entry_tail(wq, e);
	}
	set_current_state(state);
	spin_unlock(&wq->lock);
	return (was_empty);
}

/*
 * One step of the wait_event loop: -ERESTARTSYS (and e leaves the queue)
 * if a signal ends a sleep in `state`, else e is queued (once) and the
 * task's state set. The signal test runs before the queue's lock is
 * taken because it may enter illumos' signal code (src/lkpi_sched.c).
 */
long
prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e,
    int state)
{
	bool sig = signal_pending_state(state, current) != 0;
	long ret = 0;

	spin_lock(&wq->lock);
	if (sig) {
		list_del_init(&e->entry);
		ret = -ERESTARTSYS;
	} else {
		if (list_empty(&e->entry)) {
			if (e->flags & WQ_FLAG_EXCLUSIVE)
				__add_wait_queue_entry_tail(wq, e);
			else
				__add_wait_queue(wq, e);
		}
		set_current_state(state);
	}
	spin_unlock(&wq->lock);
	return (ret);
}

void
finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	__set_current_state(TASK_RUNNING);
	/* A woken autoremove entry is already off the queue. */
	if (!list_empty_careful(&e->entry)) {
		spin_lock(&wq->lock);
		list_del_init(&e->entry);
		spin_unlock(&wq->lock);
	}
}

/*
 * Sleeps unless woken_wake_function() marked e since the last call; the
 * mark is consumed. With prepare/add before the condition test, a
 * wake-up after the test is never lost.
 */
long
wait_woken(struct wait_queue_entry *e, unsigned int mode, long timeout)
{
	set_current_state(mode);
	if ((__atomic_load_n(&e->flags, __ATOMIC_SEQ_CST) &
	    WQ_FLAG_WOKEN) == 0 && !kthread_should_stop())
		timeout = schedule_timeout(timeout);
	__set_current_state(TASK_RUNNING);
	(void) __atomic_fetch_and(&e->flags, ~(unsigned int)WQ_FLAG_WOKEN,
	    __ATOMIC_SEQ_CST);
	return (timeout);
}

/* --- waking ------------------------------------------------------------ */

static void
lkpi_wake_up_common(struct wait_queue_head *wq, unsigned int mode, int nr,
    void *key)
{
	struct wait_queue_entry *e, *next;
	unsigned int flags;
	int ret;

	list_for_each_entry_safe(e, next, &wq->head, entry) {
		flags = e->flags;
		ret = e->func(e, mode, 0, key);
		if (ret < 0)
			break;
		if (ret != 0 && (flags & WQ_FLAG_EXCLUSIVE) != 0 && --nr == 0)
			break;
	}
}

void
__wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
	spin_lock(&wq->lock);
	lkpi_wake_up_common(wq, mode, nr, key);
	spin_unlock(&wq->lock);
}

void
__wake_up_locked(struct wait_queue_head *wq, unsigned int mode, int nr)
{
	lkpi_wake_up_common(wq, mode, nr, NULL);
}

void
__wake_up_locked_key(struct wait_queue_head *wq, unsigned int mode, void *key)
{
	lkpi_wake_up_common(wq, mode, 1, key);
}

/* --- completions --------------------------------------------------------- */

void
complete(struct completion *x)
{
	spin_lock(&x->wait.lock);
	if (x->done != UINT_MAX)
		x->done++;
	lkpi_wake_up_common(&x->wait, TASK_NORMAL, 1, NULL);
	spin_unlock(&x->wait.lock);
}

void
complete_all(struct completion *x)
{
	spin_lock(&x->wait.lock);
	x->done = UINT_MAX;
	lkpi_wake_up_common(&x->wait, TASK_NORMAL, 0, NULL);
	spin_unlock(&x->wait.lock);
}

/*
 * Waits for x in `state` for at most timeout jiffies: the jiffies left
 * (at least 1) once done, 0 on timeout, -ERESTARTSYS on a signal.
 */
static long
lkpi_wait_for_common(struct completion *x, long timeout, int state)
{
	struct wait_queue_entry w;

	might_sleep();
	spin_lock(&x->wait.lock);
	if (x->done == 0) {
		init_waitqueue_entry(&w, current);
		w.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_entry_tail(&x->wait, &w);
		do {
			/* The signal test may enter illumos' signal code. */
			set_current_state(state);
			spin_unlock(&x->wait.lock);
			if (signal_pending_state(state, current)) {
				__set_current_state(TASK_RUNNING);
				spin_lock(&x->wait.lock);
				timeout = -ERESTARTSYS;
				break;
			}
			timeout = schedule_timeout(timeout);
			spin_lock(&x->wait.lock);
		} while (x->done == 0 && timeout != 0);
		__remove_wait_queue(&x->wait, &w);
		if (x->done == 0) {
			spin_unlock(&x->wait.lock);
			return (timeout);
		}
	}
	if (x->done != UINT_MAX)
		x->done--;
	spin_unlock(&x->wait.lock);
	return (timeout != 0 ? timeout : 1);
}

void
wait_for_completion(struct completion *x)
{
	(void) lkpi_wait_for_common(x, MAX_SCHEDULE_TIMEOUT,
	    TASK_UNINTERRUPTIBLE);
}

unsigned long
wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	return ((unsigned long)lkpi_wait_for_common(x,
	    (long)MIN(timeout, (unsigned long)MAX_SCHEDULE_TIMEOUT),
	    TASK_UNINTERRUPTIBLE));
}

int
wait_for_completion_interruptible(struct completion *x)
{
	long r = lkpi_wait_for_common(x, MAX_SCHEDULE_TIMEOUT,
	    TASK_INTERRUPTIBLE);

	return (r < 0 ? (int)r : 0);
}

long
wait_for_completion_interruptible_timeout(struct completion *x,
    unsigned long timeout)
{
	return (lkpi_wait_for_common(x,
	    (long)MIN(timeout, (unsigned long)MAX_SCHEDULE_TIMEOUT),
	    TASK_INTERRUPTIBLE));
}

int
wait_for_completion_killable(struct completion *x)
{
	long r = lkpi_wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);

	return (r < 0 ? (int)r : 0);
}

long
wait_for_completion_killable_timeout(struct completion *x,
    unsigned long timeout)
{
	return (lkpi_wait_for_common(x,
	    (long)MIN(timeout, (unsigned long)MAX_SCHEDULE_TIMEOUT),
	    TASK_KILLABLE));
}

bool
try_wait_for_completion(struct completion *x)
{
	bool ret = true;

	/* Like Linux, avoid the lock when it cannot succeed. */
	if (__atomic_load_n(&x->done, __ATOMIC_SEQ_CST) == 0)
		return (false);
	spin_lock(&x->wait.lock);
	if (x->done == 0)
		ret = false;
	else if (x->done != UINT_MAX)
		x->done--;
	spin_unlock(&x->wait.lock);
	return (ret);
}

bool
completion_done(struct completion *x)
{
	bool done;

	if (__atomic_load_n(&x->done, __ATOMIC_SEQ_CST) == 0)
		return (false);
	/* Wait for complete() to let go of the lock, as Linux does. */
	spin_lock(&x->wait.lock);
	done = x->done != 0;
	spin_unlock(&x->wait.lock);
	return (done);
}

/* --- shared wait queues for variables and bits --------------------------- */

/*
 * wait_var_event() and wait_on_bit() sleep on one of these queues, chosen
 * by address; wake_up_var() and wake_up_bit() wake every sleeper of the
 * queue, and each re-tests its own condition.
 */
#define	LKPI_VAR_WQ_SHIFT	6
#define	LKPI_VAR_WQ_COUNT	(1 << LKPI_VAR_WQ_SHIFT)

static struct wait_queue_head lkpi_var_wq[LKPI_VAR_WQ_COUNT];

void
lkpi_wait_init(void)
{
	int i;

	for (i = 0; i < LKPI_VAR_WQ_COUNT; i++)
		init_waitqueue_head(&lkpi_var_wq[i]);
}

static struct wait_queue_head *
lkpi_var_wq_hash(uintptr_t v)
{
	/* Fibonacci hashing of the address. */
	uint64_t h = (uint64_t)v * 0x61c8864680b583ebULL;

	return (&lkpi_var_wq[h >> (64 - LKPI_VAR_WQ_SHIFT)]);
}

struct wait_queue_head *
__var_waitqueue(void *p)
{
	return (lkpi_var_wq_hash((uintptr_t)p));
}

struct wait_queue_head *
bit_waitqueue(void *word, int bit)
{
	return (lkpi_var_wq_hash(((uintptr_t)word << 6) | (uintptr_t)(bit & 63)));
}

void
wake_up_var(void *var)
{
	/* Order the caller's store of the condition before the queue test. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__wake_up(__var_waitqueue(var), TASK_NORMAL, 0, NULL);
}

void
wake_up_bit(void *word, int bit)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__wake_up(bit_waitqueue(word, bit), TASK_NORMAL, 0, NULL);
}
