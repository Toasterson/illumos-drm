/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's PCI interrupt vectors and interrupt handlers over ddi_intr_*(9F)
 * (see <linux/interrupt.h> and <linux/pci.h>).
 *
 * pci_alloc_irq_vectors() allocates MSI-X, MSI or fixed interrupts of the
 * device with ddi_intr_alloc(9F) and enters each vector in a global table;
 * the Linux "irq number" of a vector is its table index + 1, so 0 is never
 * valid. request_threaded_irq() adds a trampoline as the DDI handler; it
 * holds the vector's run lock while the Linux handler runs, which is what
 * synchronize_irq() waits for. Vectors at or above
 * ddi_intr_get_hilevel_pri(9F) are refused: Linux handlers take the
 * layer's spinlocks, which are adaptive mutexes.
 *
 * Threaded handlers: when the primary handler returns IRQ_WAKE_THREAD,
 * the thread function runs on the vector's own thread (a single-thread
 * ddi_taskq(9F) created by request_threaded_irq()), in thread context as
 * on Linux, where it may sleep. Wake-ups while it runs are coalesced into
 * one more run. synchronize_irq() also waits for it.
 *
 * disable_irq() and enable_irq() nest. ddi_intr_disable(9F) and
 * ddi_intr_enable(9F) may not be called in interrupt context, and
 * disabling an MSI vector loses interrupts that arrive meanwhile, so:
 * MSI and MSI-X vectors are masked in software (the trampoline records an
 * interrupt that arrives while masked, and enable_irq() replays it on a
 * layer thread, like Linux's resend of a pending interrupt); fixed
 * (level-triggered) vectors are also disabled in the hardware, which
 * stops a level interrupt from repeating. A hardware change requested in
 * interrupt context is made by the vector's work item instead.
 *
 * free_irq() removes the handler from a disabled vector. If illumos
 * refuses to disable it or to remove the handler, the vector is "stuck":
 * the trampoline stays registered but ignores every interrupt, the vector
 * is never freed and the layer refuses to unload (lkpi_irq_stuck()).
 *
 * MSI vectors with the block-enable capability are enabled and disabled
 * one at a time as one-element blocks (ddi_intr_block_enable(9F)); i915
 * uses a single vector.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

#define	LKPI_NR_IRQS	256

struct lkpi_irq_vec {
	struct pci_dev *lv_pdev;
	ddi_intr_handle_t lv_h;
	unsigned int lv_irq;		/* table index + 1 */
	uint_t lv_pri;
	int lv_type;			/* DDI_INTR_TYPE_* */
	bool lv_block;			/* enable with ddi_intr_block_*() */
	kmutex_t lv_run;		/* held while the handler runs */
	bool lv_missed;			/* (lv_run) arrived while masked */
	kmutex_t lv_lock;		/* the fields below */
	irq_handler_t lv_handler;
	irq_handler_t lv_thread_fn;
	void *lv_dev_id;
	const char *lv_name;
	bool lv_requested;
	bool lv_hw_enabled;
	volatile bool lv_masked;	/* lv_depth != 0 */
	volatile bool lv_stuck;		/* handler could not be removed */
	unsigned int lv_depth;		/* disable_irq() nesting */
	/* The thread of a threaded handler. */
	ddi_taskq_t *lv_tq;
	kthread_t *lv_thr_thread;
	kcondvar_t lv_thr_cv;
	bool lv_thr_pending;
	bool lv_thr_running;
	bool lv_thr_exit;
	/* Hardware enable/disable and replay outside interrupt context. */
	struct work_struct lv_work;
};

/* The interrupts of a device (struct pci_dev's lkpi_irq). */
struct lkpi_pci_irq {
	int li_type;			/* DDI_INTR_TYPE_* */
	int li_alloc;			/* size of the arrays */
	int li_count;			/* allocated vectors */
	bool li_implicit;		/* INTx after a failed pci_enable_msi */
	ddi_intr_handle_t *li_h;
	struct lkpi_irq_vec **li_vec;
};

static kmutex_t lkpi_irq_lock;		/* lkpi_irq_table */
static struct lkpi_irq_vec *lkpi_irq_table[LKPI_NR_IRQS];
static uint_t lkpi_irq_hilevel;
static volatile uint_t lkpi_irq_nstuck;

int
lkpi_irq_init(void)
{
	(mutex_init)(&lkpi_irq_lock, NULL, MUTEX_DRIVER, NULL);
	lkpi_irq_hilevel = ddi_intr_get_hilevel_pri();
	return (0);
}

void
lkpi_irq_fini(void)
{
	(mutex_destroy)(&lkpi_irq_lock);
}

uint_t
lkpi_irq_stuck(void)
{
	return (__atomic_load_n(&lkpi_irq_nstuck, __ATOMIC_SEQ_CST));
}

static struct lkpi_irq_vec *
lkpi_irq_get(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	if (irq == 0 || irq > LKPI_NR_IRQS)
		return (NULL);
	mutex_enter(&lkpi_irq_lock);
	v = lkpi_irq_table[irq - 1];
	mutex_exit(&lkpi_irq_lock);
	return (v);
}

/* --- the DDI handler and the handler thread ------------------------------ */

/* Wakes the vector's thread for one (more) run of the thread function. */
static void
lkpi_irq_wake_thread(struct lkpi_irq_vec *v)
{
	mutex_enter(&v->lv_lock);
	if (v->lv_tq != NULL && !v->lv_thr_exit) {
		v->lv_thr_pending = true;
		cv_broadcast(&v->lv_thr_cv);
	}
	mutex_exit(&v->lv_lock);
}

/* Runs the Linux handler; lv_run held. */
static irqreturn_t
lkpi_irq_call(struct lkpi_irq_vec *v)
{
	irqreturn_t r;

	ASSERT(MUTEX_HELD(&v->lv_run));
	/* A threaded handler without a primary handler always wakes. */
	if (v->lv_handler != NULL)
		r = v->lv_handler((int)v->lv_irq, v->lv_dev_id);
	else
		r = IRQ_WAKE_THREAD;
	if ((r & IRQ_WAKE_THREAD) != 0 && v->lv_thread_fn != NULL)
		lkpi_irq_wake_thread(v);
	return (r);
}

static uint_t
lkpi_irq_intr(caddr_t arg1, caddr_t arg2)
{
	struct lkpi_irq_vec *v = (struct lkpi_irq_vec *)(void *)arg1;
	irqreturn_t r;

	(void) arg2;
	mutex_enter(&v->lv_run);
	if (v->lv_stuck ||
	    (v->lv_handler == NULL && v->lv_thread_fn == NULL)) {
		mutex_exit(&v->lv_run);
		return (DDI_INTR_UNCLAIMED);
	}
	if (v->lv_masked) {
		/* Disabled: delivered by enable_irq() (lkpi_irq_work()). */
		v->lv_missed = true;
		mutex_exit(&v->lv_run);
		return (DDI_INTR_CLAIMED);
	}
	r = lkpi_irq_call(v);
	mutex_exit(&v->lv_run);
	return ((r & (IRQ_HANDLED | IRQ_WAKE_THREAD)) != 0 ?
	    DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}

/* The thread of a threaded handler. */
static void
lkpi_irq_thread(void *arg)
{
	struct lkpi_irq_vec *v = arg;
	irq_handler_t fn;
	void *dev_id;

	mutex_enter(&v->lv_lock);
	v->lv_thr_thread = curthread;
	for (;;) {
		while (!v->lv_thr_pending && !v->lv_thr_exit)
			cv_wait(&v->lv_thr_cv, &v->lv_lock);
		if (!v->lv_thr_pending)
			break;
		v->lv_thr_pending = false;
		fn = v->lv_thread_fn;
		dev_id = v->lv_dev_id;
		if (fn == NULL)
			continue;
		v->lv_thr_running = true;
		mutex_exit(&v->lv_lock);
		(void) fn((int)v->lv_irq, dev_id);
		mutex_enter(&v->lv_lock);
		v->lv_thr_running = false;
		cv_broadcast(&v->lv_thr_cv);
	}
	v->lv_thr_thread = NULL;
	cv_broadcast(&v->lv_thr_cv);
	mutex_exit(&v->lv_lock);
}

/* Starts the handler thread of a threaded handler; lv_lock held. */
static int
lkpi_irq_thread_start(struct lkpi_irq_vec *v)
{
	char name[32];

	ASSERT(MUTEX_HELD(&v->lv_lock));
	(void) (snprintf)(name, sizeof (name), "lkpi_irq%u", v->lv_irq);
	v->lv_thr_pending = false;
	v->lv_thr_running = false;
	v->lv_thr_exit = false;
	v->lv_tq = ddi_taskq_create(NULL, name, 1, LKPI_PRI_HIGH, 0);
	if (v->lv_tq == NULL)
		return (-ENOMEM);
	if (ddi_taskq_dispatch(v->lv_tq, lkpi_irq_thread, v, DDI_SLEEP) !=
	    DDI_SUCCESS) {
		ddi_taskq_destroy(v->lv_tq);
		v->lv_tq = NULL;
		return (-ENOMEM);
	}
	return (0);
}

/* Stops the handler thread after a pending run; lv_lock not held. */
static void
lkpi_irq_thread_stop(struct lkpi_irq_vec *v)
{
	ddi_taskq_t *tq;

	mutex_enter(&v->lv_lock);
	tq = v->lv_tq;
	v->lv_thr_exit = true;
	cv_broadcast(&v->lv_thr_cv);
	mutex_exit(&v->lv_lock);
	if (tq != NULL)
		ddi_taskq_destroy(tq);
	mutex_enter(&v->lv_lock);
	v->lv_tq = NULL;
	mutex_exit(&v->lv_lock);
}

static int
lkpi_irq_hw_enable(struct lkpi_irq_vec *v)
{
	int rc;

	ASSERT(MUTEX_HELD(&v->lv_lock));
	if (v->lv_hw_enabled)
		return (0);
	if (v->lv_block)
		rc = ddi_intr_block_enable(&v->lv_h, 1);
	else
		rc = ddi_intr_enable(v->lv_h);
	if (rc != DDI_SUCCESS)
		return (-EIO);
	v->lv_hw_enabled = true;
	return (0);
}

/* 0, or -EIO if illumos refused (the vector stays enabled). */
static int
lkpi_irq_hw_disable(struct lkpi_irq_vec *v)
{
	int rc;

	ASSERT(MUTEX_HELD(&v->lv_lock));
	if (!v->lv_hw_enabled)
		return (0);
	if (v->lv_block)
		rc = ddi_intr_block_disable(&v->lv_h, 1);
	else
		rc = ddi_intr_disable(v->lv_h);
	if (rc != DDI_SUCCESS) {
		cmn_err(CE_WARN, "linuxkpi: cannot disable irq %u",
		    v->lv_irq);
		return (-EIO);
	}
	v->lv_hw_enabled = false;
	return (0);
}

/*
 * The vector's work item (thread context): brings the hardware state in
 * line with the disable depth (fixed vectors are disabled while the
 * depth is non-zero, every vector is enabled at depth 0), then delivers
 * an interrupt that arrived while the vector was masked.
 */
static void
lkpi_irq_work(struct work_struct *w)
{
	struct lkpi_irq_vec *v = container_of(w, struct lkpi_irq_vec, lv_work);

	mutex_enter(&v->lv_lock);
	if (v->lv_requested && !v->lv_stuck) {
		if (v->lv_depth == 0) {
			if (lkpi_irq_hw_enable(v) != 0)
				cmn_err(CE_WARN, "linuxkpi: cannot enable irq "
				    "%u", v->lv_irq);
		} else if (v->lv_type == DDI_INTR_TYPE_FIXED) {
			(void) lkpi_irq_hw_disable(v);
		}
	}
	mutex_exit(&v->lv_lock);

	mutex_enter(&v->lv_run);
	if (v->lv_missed && !v->lv_masked && !v->lv_stuck &&
	    (v->lv_handler != NULL || v->lv_thread_fn != NULL)) {
		v->lv_missed = false;
		(void) lkpi_irq_call(v);
	}
	mutex_exit(&v->lv_run);
}

/* --- handlers ---------------------------------------------------------- */

int
request_threaded_irq(unsigned int irq, irq_handler_t handler,
    irq_handler_t thread_fn, unsigned long flags, const char *name,
    void *dev_id)
{
	struct lkpi_irq_vec *v;
	int ret;

	(void) flags;
	/*
	 * Threaded handlers are refused: their hand-off takes lv_lock in the
	 * interrupt path, and free_irq()/disable_irq() hold it across
	 * ddi_intr_disable(9F), which waits for an interrupt in progress on
	 * the vector (a blocked interrupt thread would never let it finish).
	 * i915 uses none; a driver that needs one gets -EOPNOTSUPP rather than
	 * a hang.
	 */
	if (thread_fn != NULL) {
		WARN_ONCE(1, "request_threaded_irq: threaded handlers are not "
		    "supported (irq %u)", irq);
		return (-EOPNOTSUPP);
	}
	if (handler == NULL)
		return (-EINVAL);
	if (servicing_interrupt())
		return (-EINVAL);
	v = lkpi_irq_get(irq);
	if (v == NULL)
		return (-EINVAL);
	if (v->lv_pri >= lkpi_irq_hilevel) {
		cmn_err(CE_WARN, "linuxkpi: irq %u has high-level priority %u, "
		    "refused", irq, v->lv_pri);
		return (-EINVAL);
	}

	mutex_enter(&v->lv_lock);
	if (v->lv_requested || v->lv_stuck) {
		mutex_exit(&v->lv_lock);
		return (-EBUSY);
	}
	v->lv_handler = handler;
	v->lv_thread_fn = thread_fn;
	v->lv_dev_id = dev_id;
	v->lv_name = name;
	v->lv_depth = 0;
	v->lv_masked = false;
	v->lv_missed = false;
	if (thread_fn != NULL) {
		ret = lkpi_irq_thread_start(v);
		if (ret != 0)
			goto fail;
	}
	if (ddi_intr_add_handler(v->lv_h, lkpi_irq_intr, v, NULL) !=
	    DDI_SUCCESS) {
		ret = -EIO;
		goto fail_thread;
	}
	ret = lkpi_irq_hw_enable(v);
	if (ret != 0) {
		(void) ddi_intr_remove_handler(v->lv_h);
		goto fail_thread;
	}
	v->lv_requested = true;
	mutex_exit(&v->lv_lock);
	return (0);

fail_thread:
	if (v->lv_tq != NULL) {
		mutex_exit(&v->lv_lock);
		lkpi_irq_thread_stop(v);
		mutex_enter(&v->lv_lock);
	}
fail:
	v->lv_handler = NULL;
	v->lv_thread_fn = NULL;
	v->lv_dev_id = NULL;
	v->lv_name = NULL;
	mutex_exit(&v->lv_lock);
	return (ret);
}

/* Waits until neither the handler nor its thread function runs. */
static void
lkpi_irq_wait_idle(struct lkpi_irq_vec *v)
{
	mutex_enter(&v->lv_run);
	mutex_exit(&v->lv_run);
	mutex_enter(&v->lv_lock);
	while (v->lv_tq != NULL && (v->lv_thr_pending || v->lv_thr_running))
		cv_wait(&v->lv_thr_cv, &v->lv_lock);
	mutex_exit(&v->lv_lock);
}

const void *
free_irq(unsigned int irq, void *dev_id)
{
	struct lkpi_irq_vec *v;
	const char *name;
	bool stuck = false;

	v = lkpi_irq_get(irq);
	if (v == NULL) {
		WARN(1, "free_irq: irq %u not allocated", irq);
		return (NULL);
	}
	/* Removing the handler from inside it would wait for itself. */
	if (servicing_interrupt() || mutex_owned(&v->lv_run) ||
	    v->lv_thr_thread == curthread) {
		WARN(1, "free_irq: irq %u freed from interrupt context or its "
		    "own handler", irq);
		return (NULL);
	}
	mutex_enter(&v->lv_lock);
	if (!v->lv_requested || v->lv_dev_id != dev_id) {
		mutex_exit(&v->lv_lock);
		WARN(1, "free_irq: irq %u not requested by this dev_id", irq);
		return (NULL);
	}
	/* ddi_intr_remove_handler(9F) requires a disabled vector. */
	if (lkpi_irq_hw_disable(v) != 0)
		stuck = true;
	v->lv_requested = false;
	name = v->lv_name;
	mutex_exit(&v->lv_lock);

	/* Outside lv_lock: removal may wait for a running handler. */
	if (!stuck && ddi_intr_remove_handler(v->lv_h) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "linuxkpi: cannot remove the handler of "
		    "irq %u", irq);
		stuck = true;
	}
	if (stuck) {
		/* The trampoline stays registered and ignores interrupts. */
		v->lv_stuck = true;
		(void) __atomic_add_fetch(&lkpi_irq_nstuck, 1,
		    __ATOMIC_SEQ_CST);
		WARN(1, "free_irq: irq %u stays registered; the layer cannot "
		    "unload", irq);
	}
	lkpi_irq_wait_idle(v);
	lkpi_irq_thread_stop(v);
	(void) cancel_work_sync(&v->lv_work);

	mutex_enter(&v->lv_run);
	mutex_enter(&v->lv_lock);
	v->lv_handler = NULL;
	v->lv_thread_fn = NULL;
	v->lv_dev_id = NULL;
	v->lv_name = NULL;
	v->lv_depth = 0;
	v->lv_masked = false;
	v->lv_missed = false;
	mutex_exit(&v->lv_lock);
	mutex_exit(&v->lv_run);
	return (name);
}

void
synchronize_irq(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	v = lkpi_irq_get(irq);
	if (v == NULL)
		return;
	if (servicing_interrupt() || mutex_owned(&v->lv_run) ||
	    v->lv_thr_thread == curthread) {
		WARN(1, "synchronize_irq: irq %u from interrupt context or its "
		    "own handler", irq);
		return;
	}
	lkpi_irq_wait_idle(v);
}

void
synchronize_hardirq(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	v = lkpi_irq_get(irq);
	if (v == NULL || mutex_owned(&v->lv_run) || servicing_interrupt())
		return;
	mutex_enter(&v->lv_run);
	mutex_exit(&v->lv_run);
}

void
disable_irq_nosync(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	v = lkpi_irq_get(irq);
	if (v == NULL)
		return;
	mutex_enter(&v->lv_lock);
	if (v->lv_depth++ == 0) {
		v->lv_masked = true;
		if (v->lv_requested && v->lv_type == DDI_INTR_TYPE_FIXED) {
			/*
			 * From an interrupt, or from a replayed handler that
			 * holds lv_run (lkpi_irq_work()): the hardware change
			 * waits for interrupts in progress, which may be
			 * waiting for lv_run, so it is deferred.
			 */
			if (servicing_interrupt() || mutex_owned(&v->lv_run))
				(void) queue_work(system_highpri_wq,
				    &v->lv_work);
			else
				(void) lkpi_irq_hw_disable(v);
		}
	}
	mutex_exit(&v->lv_lock);
}

void
disable_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	if (!servicing_interrupt())
		synchronize_irq(irq);
}

void
enable_irq(unsigned int irq)
{
	struct lkpi_irq_vec *v;
	bool kick = false;

	v = lkpi_irq_get(irq);
	if (v == NULL)
		return;
	mutex_enter(&v->lv_lock);
	if (v->lv_depth == 0) {
		mutex_exit(&v->lv_lock);
		WARN(1, "enable_irq: irq %u not disabled", irq);
		return;
	}
	if (--v->lv_depth == 0 && v->lv_requested) {
		v->lv_masked = false;
		if (!v->lv_hw_enabled && !servicing_interrupt() &&
		    lkpi_irq_hw_enable(v) != 0)
			cmn_err(CE_WARN, "linuxkpi: cannot enable irq %u", irq);
		/*
		 * The work item enables the hardware if this could not, and
		 * delivers an interrupt that arrived while masked (it checks
		 * under lv_run, after the trampoline saw the mask).
		 */
		kick = true;
	} else if (v->lv_depth == 0) {
		v->lv_masked = false;
	}
	if (kick)
		(void) queue_work(system_highpri_wq, &v->lv_work);
	mutex_exit(&v->lv_lock);
}

/* --- vectors ----------------------------------------------------------- */

static void
lkpi_irq_free_li(struct lkpi_pci_irq *li, int nvec)
{
	int i;

	for (i = 0; i < nvec; i++) {
		struct lkpi_irq_vec *v = li->li_vec[i];

		if (v == NULL)
			continue;
		if (v->lv_irq != 0) {
			mutex_enter(&lkpi_irq_lock);
			lkpi_irq_table[v->lv_irq - 1] = NULL;
			mutex_exit(&lkpi_irq_lock);
		}
		/*
		 * A stuck vector's handler still points at v: v and its
		 * handle stay allocated (li_vec[i] marks it below).
		 */
		if (v->lv_stuck)
			continue;
		cv_destroy(&v->lv_thr_cv);
		(mutex_destroy)(&v->lv_lock);
		(mutex_destroy)(&v->lv_run);
		kmem_free(v, sizeof (*v));
		li->li_vec[i] = NULL;
	}
	for (i = 0; i < li->li_count; i++) {
		if (li->li_vec[i] == NULL)
			(void) ddi_intr_free(li->li_h[i]);
	}
	kmem_free(li->li_vec, li->li_alloc * sizeof (*li->li_vec));
	kmem_free(li->li_h, li->li_alloc * sizeof (*li->li_h));
	kmem_free(li, sizeof (*li));
}

/*
 * Allocates between min and max vectors of one type; the count, or
 * -ENOSPC.
 */
static int
lkpi_irq_alloc(struct pci_dev *pdev, int type, unsigned int min,
    unsigned int max)
{
	dev_info_t *dip = pdev->dev.lkpi_dip;
	struct lkpi_pci_irq *li;
	int nintrs, navail, count, actual, cap, i, slot;

	if (ddi_intr_get_nintrs(dip, type, &nintrs) != DDI_SUCCESS ||
	    nintrs < 1)
		return (-ENOSPC);
	if (ddi_intr_get_navail(dip, type, &navail) != DDI_SUCCESS ||
	    navail < 1)
		return (-ENOSPC);
	count = MIN(nintrs, navail);
	if ((unsigned int)count > max)
		count = (int)max;
	/* MSI allocates a power of two. */
	if (type == DDI_INTR_TYPE_MSI) {
		while ((count & (count - 1)) != 0)
			count &= count - 1;
	}
	if ((unsigned int)count < min)
		return (-ENOSPC);

	li = kmem_zalloc(sizeof (*li), KM_SLEEP);
	li->li_type = type;
	li->li_alloc = count;
	li->li_h = kmem_zalloc(count * sizeof (*li->li_h), KM_SLEEP);
	li->li_vec = kmem_zalloc(count * sizeof (*li->li_vec), KM_SLEEP);
	if (ddi_intr_alloc(dip, li->li_h, type, 0, count, &actual,
	    DDI_INTR_ALLOC_NORMAL) != DDI_SUCCESS)
		actual = 0;
	li->li_count = actual;
	if ((unsigned int)actual < min || actual == 0) {
		lkpi_irq_free_li(li, 0);
		return (-ENOSPC);
	}
	if (ddi_intr_get_cap(li->li_h[0], &cap) != DDI_SUCCESS)
		cap = 0;

	for (i = 0; i < actual; i++) {
		struct lkpi_irq_vec *v;
		uint_t pri;

		if (ddi_intr_get_pri(li->li_h[i], &pri) != DDI_SUCCESS) {
			lkpi_irq_free_li(li, i);
			return (-EIO);
		}
		v = kmem_zalloc(sizeof (*v), KM_SLEEP);
		v->lv_pdev = pdev;
		v->lv_h = li->li_h[i];
		v->lv_pri = pri;
		v->lv_type = type;
		v->lv_block = (type == DDI_INTR_TYPE_MSI &&
		    (cap & DDI_INTR_FLAG_BLOCK) != 0);
		(mutex_init)(&v->lv_run, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(pri));
		(mutex_init)(&v->lv_lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(pri));
		cv_init(&v->lv_thr_cv, NULL, CV_DRIVER, NULL);
		INIT_WORK(&v->lv_work, lkpi_irq_work);
		li->li_vec[i] = v;
	}

	mutex_enter(&lkpi_irq_lock);
	for (i = 0, slot = 0; i < actual; i++) {
		while (slot < LKPI_NR_IRQS && lkpi_irq_table[slot] != NULL)
			slot++;
		if (slot == LKPI_NR_IRQS)
			break;
		lkpi_irq_table[slot] = li->li_vec[i];
		li->li_vec[i]->lv_irq = (unsigned int)slot + 1;
	}
	mutex_exit(&lkpi_irq_lock);
	if (i < actual) {
		cmn_err(CE_WARN, "linuxkpi: irq table full");
		lkpi_irq_free_li(li, actual);
		return (-ENOSPC);
	}

	pdev->lkpi_irq = li;
	pdev->irq = (int)li->li_vec[0]->lv_irq;
	pdev->msi_enabled = (type == DDI_INTR_TYPE_MSI);
	pdev->msix_enabled = (type == DDI_INTR_TYPE_MSIX);
	return (actual);
}

static bool
lkpi_irq_any_requested(const struct lkpi_pci_irq *li)
{
	int i;

	for (i = 0; i < li->li_count; i++) {
		if (li->li_vec[i]->lv_requested)
			return (true);
	}
	return (false);
}

/* Frees the device's vectors, first any handler the driver left behind. */
static void
lkpi_irq_release(struct pci_dev *pdev)
{
	struct lkpi_pci_irq *li = pdev->lkpi_irq;
	int i;

	if (li == NULL)
		return;
	for (i = 0; i < li->li_count; i++) {
		struct lkpi_irq_vec *v = li->li_vec[i];

		if (v->lv_requested) {
			WARN(1, "irq %u still has a handler, freeing it",
			    v->lv_irq);
			(void) free_irq(v->lv_irq, v->lv_dev_id);
		}
	}
	pdev->lkpi_irq = NULL;
	pdev->irq = 0;
	pdev->msi_enabled = 0;
	pdev->msix_enabled = 0;
	lkpi_irq_free_li(li, li->li_count);
}

int
pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
    unsigned int max_vecs, unsigned int flags)
{
	int types, ret = -ENOSPC;

	if (dev == NULL || dev->lkpi_foreign || dev->dev.lkpi_dip == NULL ||
	    min_vecs == 0 || min_vecs > max_vecs)
		return (-EINVAL);
	if (dev->lkpi_irq != NULL) {
		if (!dev->lkpi_irq->li_implicit ||
		    lkpi_irq_any_requested(dev->lkpi_irq))
			return (-EINVAL);
		lkpi_irq_release(dev);
	}
	if (ddi_intr_get_supported_types(dev->dev.lkpi_dip, &types) !=
	    DDI_SUCCESS)
		return (-ENODEV);
	if ((flags & PCI_IRQ_MSIX) != 0 && (types & DDI_INTR_TYPE_MSIX) != 0) {
		ret = lkpi_irq_alloc(dev, DDI_INTR_TYPE_MSIX, min_vecs,
		    max_vecs);
		if (ret > 0)
			return (ret);
	}
	if ((flags & PCI_IRQ_MSI) != 0 && (types & DDI_INTR_TYPE_MSI) != 0) {
		ret = lkpi_irq_alloc(dev, DDI_INTR_TYPE_MSI, min_vecs,
		    max_vecs);
		if (ret > 0)
			return (ret);
	}
	if ((flags & PCI_IRQ_INTX) != 0 && (types & DDI_INTR_TYPE_FIXED) != 0 &&
	    min_vecs == 1) {
		ret = lkpi_irq_alloc(dev, DDI_INTR_TYPE_FIXED, 1, 1);
		if (ret > 0)
			return (ret);
	}
	return (ret);
}

void
pci_free_irq_vectors(struct pci_dev *dev)
{
	if (dev == NULL)
		return;
	lkpi_irq_release(dev);
}

int
pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	struct lkpi_pci_irq *li;

	if (dev == NULL || (li = dev->lkpi_irq) == NULL ||
	    nr >= (unsigned int)li->li_count)
		return (-EINVAL);
	return ((int)li->li_vec[nr]->lv_irq);
}

/*
 * On Linux pdev->irq is the INTx line whenever MSI is off, and drivers
 * (i915) fall back to it when pci_enable_msi() fails. Here a failed
 * pci_enable_msi() allocates the fixed interrupt for pdev->irq; a later
 * pci_alloc_irq_vectors() replaces it.
 */
int
pci_enable_msi(struct pci_dev *dev)
{
	int types, ret;

	if (dev == NULL || dev->lkpi_foreign || dev->lkpi_irq != NULL)
		return (-EINVAL);
	ret = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
	if (ret > 0)
		return (0);
	if (ddi_intr_get_supported_types(dev->dev.lkpi_dip, &types) ==
	    DDI_SUCCESS && (types & DDI_INTR_TYPE_FIXED) != 0 &&
	    lkpi_irq_alloc(dev, DDI_INTR_TYPE_FIXED, 1, 1) > 0)
		dev->lkpi_irq->li_implicit = true;
	return (ret);
}

void
pci_disable_msi(struct pci_dev *dev)
{
	pci_free_irq_vectors(dev);
}

static int
lkpi_vec_count(struct pci_dev *dev, int type)
{
	int n;

	if (dev == NULL || dev->lkpi_foreign || dev->dev.lkpi_dip == NULL)
		return (-EINVAL);
	if (ddi_intr_get_nintrs(dev->dev.lkpi_dip, type, &n) != DDI_SUCCESS ||
	    n <= 0)
		return (-EINVAL);
	return (n);
}

int
pci_msi_vec_count(struct pci_dev *dev)
{
	return (lkpi_vec_count(dev, DDI_INTR_TYPE_MSI));
}

int
pci_msix_vec_count(struct pci_dev *dev)
{
	return (lkpi_vec_count(dev, DDI_INTR_TYPE_MSIX));
}
