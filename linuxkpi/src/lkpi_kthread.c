/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kernel threads (<linux/kthread.h>).
 *
 * Each kthread is the single thread of its own ddi_taskq(9F) (the DDI
 * has no thread_create()), running one task: it binds the kthread's task
 * as `current`, sleeps in TASK_UNINTERRUPTIBLE until the first
 * wake_up_process(), calls the function (unless kthread_stop() came
 * first: -EINTR, as on Linux) and records its result. The kthread record
 * (with the task) lives until kthread_stop() collects the result and
 * destroys the task queue; kthreads never stopped are stopped at module
 * unload.
 */

#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

struct lkpi_kthread {
	struct task_struct task;	/* task.kthread_data points here */
	int (*fn)(void *);
	void *data;
	ddi_taskq_t *tq;
	volatile uint32_t should_stop;
	boolean_t exited;
	int ret;
	kmutex_t lock;			/* exited, ret */
	kcondvar_t cv;
	struct list_head link;
};

static kmutex_t lkpi_kthread_lock;	/* zero-filled: a valid mutex */
static struct list_head lkpi_kthreads = LIST_HEAD_INIT(lkpi_kthreads);

static void
lkpi_kthread_main(void *arg)
{
	struct lkpi_kthread *kt = arg;
	struct task_struct *t = &kt->task;
	int ret;

	lkpi_task_bind(t);
	mutex_enter(&t->lock);
	while (t->__state != TASK_RUNNING)
		cv_wait(&t->cv, &t->lock);
	mutex_exit(&t->lock);

	if (__atomic_load_n(&kt->should_stop, __ATOMIC_SEQ_CST) != 0)
		ret = -EINTR;
	else
		ret = kt->fn(kt->data);

	lkpi_task_unbind();
	mutex_enter(&kt->lock);
	kt->ret = ret;
	kt->exited = B_TRUE;
	cv_broadcast(&kt->cv);
	mutex_exit(&kt->lock);
}

static void
lkpi_kthread_free(struct lkpi_kthread *kt)
{
	cv_destroy(&kt->cv);
	(mutex_destroy)(&kt->lock);
	lkpi_task_fini(&kt->task);
	kmem_free(kt, sizeof (*kt));
}

struct task_struct *
lkpi_kthread_create(int (*fn)(void *data), void *data, const char *namefmt,
    ...)
{
	char tqname[TASKQ_NAMELEN + 1];
	struct lkpi_kthread *kt;
	char name[32];
	va_list ap;

	might_sleep();
	va_start(ap, namefmt);
	(void) vsnprintf(name, sizeof (name), namefmt, ap);
	va_end(ap);

	kt = kmem_zalloc(sizeof (*kt), KM_SLEEP);
	lkpi_task_init(&kt->task, name);
	kt->task.__state = TASK_UNINTERRUPTIBLE;
	kt->task.flags = PF_KTHREAD;
	kt->task.kthread_data = kt;
	kt->fn = fn;
	kt->data = data;
	(mutex_init)(&kt->lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&kt->cv, NULL, CV_DRIVER, NULL);

	lkpi_taskq_name(tqname, sizeof (tqname), name);
	kt->tq = ddi_taskq_create(NULL, tqname, 1, TASKQ_DEFAULTPRI, 0);
	if (kt->tq == NULL) {
		lkpi_kthread_free(kt);
		return (ERR_PTR(-ENOMEM));
	}
	if (ddi_taskq_dispatch(kt->tq, lkpi_kthread_main, kt, DDI_SLEEP) !=
	    DDI_SUCCESS) {
		ddi_taskq_destroy(kt->tq);
		lkpi_kthread_free(kt);
		return (ERR_PTR(-ENOMEM));
	}
	mutex_enter(&lkpi_kthread_lock);
	list_add_tail(&kt->link, &lkpi_kthreads);
	mutex_exit(&lkpi_kthread_lock);
	return (&kt->task);
}

int
kthread_stop(struct task_struct *k)
{
	struct lkpi_kthread *kt;
	int ret;

	if (WARN_ON(k == NULL || k->kthread_data == NULL))
		return (-EINVAL);
	might_sleep();
	kt = k->kthread_data;
	if (WARN_ON(k == current))
		return (-EDEADLK);

	(void) __atomic_store_n(&kt->should_stop, 1, __ATOMIC_SEQ_CST);
	(void) wake_up_process(k);
	mutex_enter(&kt->lock);
	while (!kt->exited)
		cv_wait(&kt->cv, &kt->lock);
	ret = kt->ret;
	mutex_exit(&kt->lock);

	mutex_enter(&lkpi_kthread_lock);
	list_del(&kt->link);
	mutex_exit(&lkpi_kthread_lock);
	ddi_taskq_destroy(kt->tq);
	lkpi_kthread_free(kt);
	return (ret);
}

bool
kthread_should_stop(void)
{
	struct lkpi_kthread *kt = current->kthread_data;

	return (kt != NULL &&
	    __atomic_load_n(&kt->should_stop, __ATOMIC_SEQ_CST) != 0);
}

bool
kthread_should_park(void)
{
	return (false);
}

void
kthread_parkme(void)
{
}

int
kthread_park(struct task_struct *k)
{
	return (-ENOSYS);
}

void
kthread_unpark(struct task_struct *k)
{
}

void
lkpi_kthread_fini(void)
{
	struct lkpi_kthread *kt;
	boolean_t exited;

	mutex_enter(&lkpi_kthread_lock);
	while (!list_empty(&lkpi_kthreads)) {
		kt = list_first_entry(&lkpi_kthreads, struct lkpi_kthread,
		    link);
		mutex_exit(&lkpi_kthread_lock);
		mutex_enter(&kt->lock);
		exited = kt->exited;
		mutex_exit(&kt->lock);
		if (!exited)
			cmn_err(CE_WARN, "linuxkpi: stopping kthread \"%s\" "
			    "at unload", kt->task.comm);
		(void) kthread_stop(&kt->task);
		mutex_enter(&lkpi_kthread_lock);
	}
	mutex_exit(&lkpi_kthread_lock);
}

/*
 * Stops the kthreads whose function lies in [lo, hi), the text of a
 * driver that is unloading (lkpi_module_quiesce()). One still running is
 * a driver bug and is reported; kthread_stop() waits for it to return.
 * Returns how many were stopped.
 */
int
lkpi_kthread_quiesce(uintptr_t lo, uintptr_t hi)
{
	struct lkpi_kthread *kt, *found;
	boolean_t exited;
	int n = 0;

	for (;;) {
		found = NULL;
		mutex_enter(&lkpi_kthread_lock);
		list_for_each_entry(kt, &lkpi_kthreads, link) {
			if ((uintptr_t)kt->fn >= lo && (uintptr_t)kt->fn < hi) {
				found = kt;
				break;
			}
		}
		mutex_exit(&lkpi_kthread_lock);
		if (found == NULL)
			return (n);
		mutex_enter(&found->lock);
		exited = found->exited;
		mutex_exit(&found->lock);
		if (!exited)
			cmn_err(CE_WARN, "linuxkpi: kthread \"%s\" still "
			    "running at driver unload, stopping it",
			    found->task.comm);
		(void) kthread_stop(&found->task);
		n++;
	}
}

static volatile uint32_t lkpi_taskq_seq;

void
lkpi_taskq_name(char *buf, size_t len, const char *base)
{
	char suffix[12];
	size_t i, n;

	(void) snprintf(suffix, sizeof (suffix), "_%u",
	    atomic_inc_32_nv(&lkpi_taskq_seq));
	n = len - 1 - strlen(suffix);
	for (i = 0; i < n && base != NULL && base[i] != '\0'; i++) {
		char c = base[i];

		buf[i] = (isalnum(c) || c == '_' || c == '-' || c == '.') ?
		    c : '_';
	}
	(void) strlcpy(buf + i, suffix, len - i);
}


/* --- kthread workers ----------------------------------------------------- */

/*
 * Linux's kthread_worker API (<linux/kthread.h>), on the kthreads above;
 * an independent implementation of Linux's documented semantics.
 *
 * The worker's kthread runs lkpi_kthread_worker_fn(): it takes the first
 * work off work_list under worker->lock, records it in current_work and
 * runs it with the lock dropped, with the worker's task as `current`.
 * current_work keeps pointing at the work that ran last until the thread
 * next takes the lock; it is only compared, never dereferenced (the
 * function may free its work). A work is queued while its node is on
 * the list; work->worker is the worker it was first queued on.
 *
 * Flushing uses a barrier work with a completion on the flusher's stack,
 * as Linux does: it is queued right behind the work if that is queued, at
 * the head of the list if the work is running, at the tail for
 * kthread_flush_worker(). Works run in list order, so the barrier's
 * completion means that everything ahead of it has run.
 * kthread_cancel_work_sync() raises work->canceling while it waits for a
 * running instance, so kthread_queue_work() refuses the work meanwhile
 * (also when the work requeues itself).
 *
 * Deviations from Linux (which warns and goes on):
 * - kthread_queue_work() refuses (WARN, false) a work that belongs to
 *   another worker, rather than linking it under a lock that does not
 *   protect the list it may still be on; kthread_init_work() frees it.
 * - Flushing or cancelling a work from its own worker's thread, or
 *   flushing the worker from it, would wait forever: it WARNs and
 *   returns without waiting.
 * - kthread_destroy_worker() WARNs about works still queued after its
 *   flush (a work that requeued itself) and detaches them, so a later
 *   flush or cancel of such a work finds no worker instead of freed
 *   memory.
 * flags (KTW_FREEZABLE) has no effect: illumos does not freeze threads.
 */

struct lkpi_kthread_flush {
	struct kthread_work work;
	struct completion done;
};

static void
lkpi_kthread_flush_fn(struct kthread_work *work)
{
	struct lkpi_kthread_flush *f = container_of(work,
	    struct lkpi_kthread_flush, work);

	complete(&f->done);
}

static void
lkpi_kthread_flush_init(struct lkpi_kthread_flush *f)
{
	kthread_init_work(&f->work, lkpi_kthread_flush_fn);
	init_completion(&f->done);
}

static int
lkpi_kthread_worker_fn(void *arg)
{
	struct kthread_worker *worker = arg;
	struct kthread_work *work;

	for (;;) {
		/*
		 * The state is set before the stop flag and the list are
		 * tested: a kthread_stop() or a queueing after the tests
		 * sets it back to TASK_RUNNING, so schedule() returns.
		 */
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			spin_lock(&worker->lock);
			worker->task = NULL;
			worker->current_work = NULL;
			spin_unlock(&worker->lock);
			return (0);
		}

		work = NULL;
		spin_lock(&worker->lock);
		if (!list_empty(&worker->work_list)) {
			work = list_first_entry(&worker->work_list,
			    struct kthread_work, node);
			list_del_init(&work->node);
		}
		worker->current_work = work;
		spin_unlock(&worker->lock);

		if (work != NULL) {
			__set_current_state(TASK_RUNNING);
			work->func(work);
		} else {
			schedule();
		}
		(void) cond_resched();
	}
}

/*
 * Links work in front of pos. The thread is woken only when it is not
 * running a work: otherwise it looks at the list again when that work
 * returns. Waking a created but never started worker starts it, as on
 * Linux. Called with worker->lock held.
 */
static void
lkpi_kthread_insert_work(struct kthread_worker *worker,
    struct kthread_work *work, struct list_head *pos)
{
	assert_spin_locked(&worker->lock);
	list_add_tail(&work->node, pos);
	work->worker = worker;
	if (worker->current_work == NULL && worker->task != NULL)
		(void) wake_up_process(worker->task);
}

static struct kthread_worker *
lkpi_kthread_create_worker(unsigned int flags, const char *namefmt,
    va_list ap)
{
	struct kthread_worker *worker;
	struct task_struct *task;
	char name[32];

	might_sleep();
	(void) vsnprintf(name, sizeof (name), namefmt, ap);

	worker = kmem_zalloc(sizeof (*worker), KM_SLEEP);
	worker->flags = flags;
	spin_lock_init(&worker->lock);
	INIT_LIST_HEAD(&worker->work_list);
	INIT_LIST_HEAD(&worker->delayed_work_list);

	task = lkpi_kthread_create(lkpi_kthread_worker_fn, worker, "%s", name);
	if (IS_ERR(task)) {
		kmem_free(worker, sizeof (*worker));
		return (ERR_CAST(task));
	}
	/* Nothing can queue before the worker is returned. */
	worker->task = task;
	return (worker);
}

struct kthread_worker *
kthread_create_worker(unsigned int flags, const char *namefmt, ...)
{
	struct kthread_worker *worker;
	va_list ap;

	va_start(ap, namefmt);
	worker = lkpi_kthread_create_worker(flags, namefmt, ap);
	va_end(ap);
	return (worker);
}

struct kthread_worker *
kthread_run_worker(unsigned int flags, const char *namefmt, ...)
{
	struct kthread_worker *worker;
	va_list ap;

	va_start(ap, namefmt);
	worker = lkpi_kthread_create_worker(flags, namefmt, ap);
	va_end(ap);
	if (!IS_ERR(worker))
		(void) wake_up_process(worker->task);
	return (worker);
}

bool
kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
	bool queued = false;

	if (WARN_ON(worker == NULL || work == NULL))
		return (false);
	spin_lock(&worker->lock);
	if (WARN_ON_ONCE(work->worker != NULL && work->worker != worker)) {
		spin_unlock(&worker->lock);
		return (false);
	}
	if (list_empty(&work->node) && work->canceling == 0) {
		lkpi_kthread_insert_work(worker, work, &worker->work_list);
		queued = true;
	}
	spin_unlock(&worker->lock);
	return (queued);
}

void
kthread_flush_work(struct kthread_work *work)
{
	struct lkpi_kthread_flush fwork;
	struct kthread_worker *worker;
	bool wait = true;

	might_sleep();
	if (WARN_ON(work == NULL))
		return;
	worker = READ_ONCE(work->worker);
	if (worker == NULL)
		return;

	lkpi_kthread_flush_init(&fwork);
	spin_lock(&worker->lock);
	if (WARN_ON_ONCE(work->worker != worker)) {
		wait = false;
	} else if (!list_empty(&work->node)) {
		if (WARN_ON(worker->task == current))
			wait = false;
		else
			lkpi_kthread_insert_work(worker, &fwork.work,
			    work->node.next);
	} else if (worker->current_work == work) {
		if (WARN_ON(worker->task == current))
			wait = false;
		else
			lkpi_kthread_insert_work(worker, &fwork.work,
			    worker->work_list.next);
	} else {
		wait = false;
	}
	spin_unlock(&worker->lock);

	if (wait)
		wait_for_completion(&fwork.done);
}

void
kthread_flush_worker(struct kthread_worker *worker)
{
	struct lkpi_kthread_flush fwork;

	might_sleep();
	if (WARN_ON(worker == NULL) || WARN_ON(worker->task == current))
		return;
	lkpi_kthread_flush_init(&fwork);
	(void) kthread_queue_work(worker, &fwork.work);
	wait_for_completion(&fwork.done);
}

bool
kthread_cancel_work_sync(struct kthread_work *work)
{
	struct kthread_worker *worker;
	bool pending = false;

	might_sleep();
	if (WARN_ON(work == NULL))
		return (false);
	worker = READ_ONCE(work->worker);
	if (worker == NULL)
		return (false);

	spin_lock(&worker->lock);
	if (WARN_ON_ONCE(work->worker != worker)) {
		spin_unlock(&worker->lock);
		return (false);
	}
	if (!list_empty(&work->node)) {
		list_del_init(&work->node);
		pending = true;
	}
	if (worker->current_work == work) {
		/* kthread_flush_work() refuses to wait on its own worker. */
		work->canceling++;
		spin_unlock(&worker->lock);
		kthread_flush_work(work);
		spin_lock(&worker->lock);
		work->canceling--;
	}
	spin_unlock(&worker->lock);
	return (pending);
}

void
kthread_destroy_worker(struct kthread_worker *worker)
{
	struct kthread_work *work, *next;
	struct task_struct *task;

	if (WARN_ON(worker == NULL))
		return;
	task = worker->task;
	if (WARN_ON(task == NULL) || WARN_ON(task == current))
		return;

	kthread_flush_worker(worker);
	(void) kthread_stop(task);

	/* The thread is gone; nothing else may use the worker now. */
	(void) WARN_ON(!list_empty(&worker->delayed_work_list));
	if (WARN_ON(!list_empty(&worker->work_list))) {
		list_for_each_entry_safe(work, next, &worker->work_list,
		    node) {
			list_del_init(&work->node);
			work->worker = NULL;
		}
	}
	kmem_free(worker, sizeof (*worker));
}
