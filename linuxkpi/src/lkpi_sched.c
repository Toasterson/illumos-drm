/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux tasks on illumos threads (<linux/sched.h>).
 *
 * `current` is the struct task_struct of the calling thread, kept in
 * thread-specific data (tsd_create(), tsd_get(), tsd_set()): created on
 * the first use in thread context and freed by the key's destructor when
 * the thread exits (or at module unload, when tsd_destroy() runs the
 * destructor for every thread). A kthread's task belongs to its kthread
 * record (src/lkpi_kthread.c) and is only bound to the thread while the
 * kthread function runs. In an interrupt handler `current` is one shared
 * placeholder task that never sleeps and is never woken.
 *
 * Sleeping follows Linux's protocol: set_current_state() publishes the
 * sleep state, schedule_timeout() sleeps on the task's condition variable
 * while the state is not TASK_RUNNING, and wake_up_state() sets
 * TASK_RUNNING under the task's mutex and signals. A wake-up between
 * set_current_state() and schedule_timeout() therefore makes the sleep
 * return at once instead of being lost.
 */

#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/* Thread-specific data (<sys/proc.h>, which the prologue cannot include). */
extern void tsd_create(uint_t *, void (*)(void *));
extern void tsd_destroy(uint_t *);
extern void *tsd_get(uint_t);
extern int tsd_set(uint_t, void *);

/* The longest single cv_reltimedwait(9F): one day of jiffies. */
#define	LKPI_SLEEP_MAX_JIFFIES	(86400UL * HZ)

static uint_t lkpi_task_key;
static struct task_struct lkpi_irq_task;

void
lkpi_task_init(struct task_struct *t, const char *comm)
{
	bzero(t, sizeof (*t));
	t->prio = DEFAULT_PRIO;
	t->__state = TASK_RUNNING;
	(mutex_init)(&t->lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&t->cv, NULL, CV_DRIVER, NULL);
	(void) strlcpy(t->comm, comm, sizeof (t->comm));
}

void
lkpi_task_fini(struct task_struct *t)
{
	cv_destroy(&t->cv);
	(mutex_destroy)(&t->lock);
}

/* The tsd destructor: a thread with a task exits (or the layer unloads). */
static void
lkpi_task_dtor(void *arg)
{
	struct task_struct *t = arg;

	/* A kthread's task is freed by kthread_stop(), not here. */
	if (t == NULL || t->kthread_data != NULL)
		return;
	lkpi_task_fini(t);
	kmem_free(t, sizeof (*t));
}

void
lkpi_task_bind(struct task_struct *t)
{
	t->thread = curthread;
	t->pid = ddi_get_pid();
	if (lkpi_task_key == 0 || tsd_set(lkpi_task_key, t) != 0)
		cmn_err(CE_WARN, "linuxkpi: cannot bind task \"%s\"", t->comm);
}

void
lkpi_task_unbind(void)
{
	if (lkpi_task_key != 0)
		(void) tsd_set(lkpi_task_key, NULL);
}

struct task_struct *
lkpi_current(void)
{
	struct task_struct *t;

	if (servicing_interrupt() || lkpi_task_key == 0)
		return (&lkpi_irq_task);
	t = tsd_get(lkpi_task_key);
	if (t != NULL)
		return (t);

	/*
	 * `current` may be used under a spinlock; prefer an allocation that
	 * cannot block, but a thread must get its own task.
	 */
	t = kmem_alloc(sizeof (*t), KM_NOSLEEP);
	if (t == NULL)
		t = kmem_alloc(sizeof (*t), KM_SLEEP);
	lkpi_task_init(t, "illumos");
	t->thread = curthread;
	t->pid = ddi_get_pid();
	if (tsd_set(lkpi_task_key, t) != 0) {
		lkpi_task_fini(t);
		kmem_free(t, sizeof (*t));
		return (&lkpi_irq_task);
	}
	return (t);
}

void
lkpi_set_current_state(long state)
{
	struct task_struct *t = current;

	/* The interrupt placeholder is shared and always running. */
	if (t == &lkpi_irq_task)
		return;
	__atomic_store_n(&t->__state, state, __ATOMIC_SEQ_CST);
}

int
wake_up_state(struct task_struct *t, unsigned int state)
{
	int woken = 0;

	if (t == NULL || t == &lkpi_irq_task)
		return (0);
	mutex_enter(&t->lock);
	if ((t->__state & state) != 0) {
		t->__state = TASK_RUNNING;
		cv_broadcast(&t->cv);
		woken = 1;
	}
	mutex_exit(&t->lock);
	return (woken);
}

int
wake_up_process(struct task_struct *t)
{
	return (wake_up_state(t, TASK_NORMAL));
}

clock_t
lkpi_sleep_ticks(unsigned long j)
{
	if (j > LKPI_SLEEP_MAX_JIFFIES)
		j = LKPI_SLEEP_MAX_JIFFIES;
	if (j == 0)
		j = 1;
	return (lkpi_jiffies_to_ticks(j));
}

/*
 * Sleeps while the task's state is not TASK_RUNNING, at most timeout
 * jiffies (MAX_SCHEDULE_TIMEOUT: no limit). Interruptible and killable
 * states also end on a signal (sigpending is set). Returns the jiffies
 * left (0 when the time ran out); the state is TASK_RUNNING afterwards.
 */
long
schedule_timeout(long timeout)
{
	struct task_struct *t = current;
	unsigned long end, now;
	boolean_t intr;
	long left;

	if (t == &lkpi_irq_task) {
		WARN_ONCE(1, "linuxkpi: schedule_timeout() in an interrupt");
		return (0);
	}
	if (timeout < 0) {
		WARN_ONCE(1, "linuxkpi: schedule_timeout(%ld)", timeout);
		__atomic_store_n(&t->__state, TASK_RUNNING, __ATOMIC_SEQ_CST);
		return (0);
	}

	mutex_enter(&t->lock);
	intr = (t->__state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)) != 0;
	if (intr)
		t->sigpending = 0;

	if (timeout == MAX_SCHEDULE_TIMEOUT) {
		while (t->__state != TASK_RUNNING) {
			if (!intr) {
				cv_wait(&t->cv, &t->lock);
			} else if (cv_wait_sig(&t->cv, &t->lock) == 0) {
				t->sigpending = 1;
				break;
			}
		}
		left = MAX_SCHEDULE_TIMEOUT;
	} else {
		end = lkpi_jiffies() + (unsigned long)timeout;
		while (t->__state != TASK_RUNNING) {
			now = lkpi_jiffies();
			if (!time_before(now, end))
				break;
			if (!intr) {
				(void) cv_reltimedwait(&t->cv, &t->lock,
				    lkpi_sleep_ticks(end - now), TR_CLOCK_TICK);
			} else if (cv_reltimedwait_sig(&t->cv, &t->lock,
			    lkpi_sleep_ticks(end - now), TR_CLOCK_TICK) == 0) {
				t->sigpending = 1;
				break;
			}
		}
		now = lkpi_jiffies();
		left = time_before(now, end) ? (long)(end - now) : 0;
	}
	t->__state = TASK_RUNNING;
	mutex_exit(&t->lock);
	return (left);
}

void
schedule(void)
{
	(void) schedule_timeout(MAX_SCHEDULE_TIMEOUT);
}

void
io_schedule(void)
{
	(void) schedule_timeout(MAX_SCHEDULE_TIMEOUT);
}

/*
 * Whether the signal that ended t's last interruptible sleep is still
 * pending: a zero-length cv_reltimedwait_sig(9F) does not sleep and
 * returns 0 only if a signal is pending. Only the task's own thread can
 * ask; for other tasks the flag is reported as it is.
 */
int
lkpi_signal_recheck(struct task_struct *t)
{
	int pending;

	if (t == &lkpi_irq_task || t->thread != curthread ||
	    servicing_interrupt())
		return (t->sigpending);
	mutex_enter(&t->lock);
	if (cv_reltimedwait_sig(&t->cv, &t->lock, 0, TR_CLOCK_TICK) != 0)
		t->sigpending = 0;
	pending = t->sigpending;
	mutex_exit(&t->lock);
	return (pending);
}

/*
 * cond_resched(): give the processor away if illumos wants it, like
 * Linux's need_resched() test. kpreempt(KPREEMPT_SYNC) (<sys/disp.h>, not
 * part of the DDI, but the layer needs it anyway for kpreempt_enable())
 * preempts the thread voluntarily; it does nothing with preemption
 * disabled. delay(9F) is not an alternative: it always sleeps a tick,
 * and drivers call cond_resched() in loops over thousands of pages.
 */
void
lkpi_cond_resched(void)
{
	cpu_t *cp;

	if (servicing_interrupt() || curthread->t_preempt != 0)
		return;
	cp = CPU;
	if (cp->cpu_runrun || cp->cpu_kprunrun)
		kpreempt(KPREEMPT_SYNC);
}

/* --- struct pid ------------------------------------------------------ */

/*
 * A reference-counted process ID. get_task_pid() allocates one per call
 * (the layer has no per-process pid objects); it may be called under a
 * spinlock, so it does not sleep and returns NULL if memory is short,
 * which every pid function accepts.
 */
struct lx_pid {
	volatile uint32_t refs;
	pid_t nr;
};

struct lx_pid *
get_task_pid(struct task_struct *task, enum pid_type type)
{
	struct lx_pid *p;

	if (task == NULL)
		return (NULL);
	p = kmem_alloc(sizeof (*p), KM_NOSLEEP);
	if (p == NULL)
		return (NULL);
	p->refs = 1;
	p->nr = task->pid;
	return (p);
}

struct lx_pid *
get_pid(struct lx_pid *p)
{
	if (p != NULL)
		(void) __atomic_add_fetch(&p->refs, 1, __ATOMIC_SEQ_CST);
	return (p);
}

void
put_pid(struct lx_pid *p)
{
	if (p != NULL && __atomic_sub_fetch(&p->refs, 1, __ATOMIC_SEQ_CST) == 0)
		kmem_free(p, sizeof (*p));
}

pid_t
pid_nr(struct lx_pid *p)
{
	return (p != NULL ? p->nr : 0);
}

/* --- module state ---------------------------------------------------- */

int
lkpi_sched_init(void)
{
	lkpi_task_init(&lkpi_irq_task, "interrupt");
	lkpi_wait_init();
	tsd_create(&lkpi_task_key, lkpi_task_dtor);
	if (lkpi_task_key == 0) {
		lkpi_task_fini(&lkpi_irq_task);
		return (-ENOMEM);
	}
	return (0);
}

void
lkpi_sched_fini(void)
{
	lkpi_kthread_fini();
	/* Runs the destructor for every thread's task. */
	tsd_destroy(&lkpi_task_key);
	lkpi_task_fini(&lkpi_irq_task);
}

/*
 * Linux's preempt_disable()/preempt_enable() (<linux/preempt.h>): out of
 * line, so that drivers need no kpreempt().
 */
void
lkpi_preempt_disable(void)
{
	kpreempt_disable();
}

void
lkpi_preempt_enable(void)
{
	kpreempt_enable();
}

int
lkpi_in_interrupt(void)
{
	return (servicing_interrupt());
}
