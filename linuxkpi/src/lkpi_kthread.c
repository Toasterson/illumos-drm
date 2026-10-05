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

	kt->tq = ddi_taskq_create(NULL, name, 1, TASKQ_DEFAULTPRI, 0);
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
