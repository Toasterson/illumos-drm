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
 * synchronize_irq() waits for. A threaded handler's thread function runs
 * in the trampoline when the handler returns IRQ_WAKE_THREAD (no separate
 * thread). Vectors at or above ddi_intr_get_hilevel_pri(9F) are refused:
 * Linux handlers take the layer's spinlocks, which are adaptive mutexes.
 *
 * MSI vectors with the block-enable capability are enabled and disabled
 * one at a time as one-element blocks (ddi_intr_block_enable(9F)); i915
 * uses a single vector.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

#define	LKPI_NR_IRQS	256

struct lkpi_irq_vec {
	struct pci_dev *lv_pdev;
	ddi_intr_handle_t lv_h;
	unsigned int lv_irq;		/* table index + 1 */
	uint_t lv_pri;
	bool lv_block;			/* enable with ddi_intr_block_*() */
	kmutex_t lv_run;		/* held while the handler runs */
	kmutex_t lv_lock;		/* the fields below */
	irq_handler_t lv_handler;
	irq_handler_t lv_thread_fn;
	void *lv_dev_id;
	const char *lv_name;
	bool lv_requested;
	bool lv_hw_enabled;
	unsigned int lv_depth;		/* disable_irq() nesting */
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

/* --- the DDI handler ---------------------------------------------------- */

static uint_t
lkpi_irq_intr(caddr_t arg1, caddr_t arg2)
{
	struct lkpi_irq_vec *v = (struct lkpi_irq_vec *)(void *)arg1;
	irqreturn_t r;

	(void) arg2;
	mutex_enter(&v->lv_run);
	/* A threaded handler without a primary handler always wakes. */
	if (v->lv_handler != NULL)
		r = v->lv_handler((int)v->lv_irq, v->lv_dev_id);
	else
		r = IRQ_WAKE_THREAD;
	if (r == IRQ_WAKE_THREAD && v->lv_thread_fn != NULL)
		(void) v->lv_thread_fn((int)v->lv_irq, v->lv_dev_id);
	mutex_exit(&v->lv_run);
	return ((r & (IRQ_HANDLED | IRQ_WAKE_THREAD)) != 0 ?
	    DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
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

static void
lkpi_irq_hw_disable(struct lkpi_irq_vec *v)
{
	int rc;

	ASSERT(MUTEX_HELD(&v->lv_lock));
	if (!v->lv_hw_enabled)
		return;
	if (v->lv_block)
		rc = ddi_intr_block_disable(&v->lv_h, 1);
	else
		rc = ddi_intr_disable(v->lv_h);
	if (rc == DDI_SUCCESS)
		v->lv_hw_enabled = false;
	else
		cmn_err(CE_WARN, "linuxkpi: cannot disable irq %u",
		    v->lv_irq);
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
	if (handler == NULL && thread_fn == NULL)
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
	if (v->lv_requested) {
		mutex_exit(&v->lv_lock);
		return (-EBUSY);
	}
	v->lv_handler = handler;
	v->lv_thread_fn = thread_fn;
	v->lv_dev_id = dev_id;
	v->lv_name = name;
	v->lv_depth = 0;
	if (ddi_intr_add_handler(v->lv_h, lkpi_irq_intr, v, NULL) !=
	    DDI_SUCCESS) {
		ret = -EIO;
		goto fail;
	}
	ret = lkpi_irq_hw_enable(v);
	if (ret != 0) {
		(void) ddi_intr_remove_handler(v->lv_h);
		goto fail;
	}
	v->lv_requested = true;
	mutex_exit(&v->lv_lock);
	return (0);

fail:
	v->lv_handler = NULL;
	v->lv_thread_fn = NULL;
	v->lv_dev_id = NULL;
	v->lv_name = NULL;
	mutex_exit(&v->lv_lock);
	return (ret);
}

const void *
free_irq(unsigned int irq, void *dev_id)
{
	struct lkpi_irq_vec *v;
	const char *name;

	v = lkpi_irq_get(irq);
	if (v == NULL) {
		WARN(1, "free_irq: irq %u not allocated", irq);
		return (NULL);
	}
	/* Removing the handler from inside it would wait for itself. */
	if (mutex_owned(&v->lv_run)) {
		WARN(1, "free_irq: irq %u freed from its own handler", irq);
		return (NULL);
	}
	mutex_enter(&v->lv_lock);
	if (!v->lv_requested || v->lv_dev_id != dev_id) {
		mutex_exit(&v->lv_lock);
		WARN(1, "free_irq: irq %u not requested by this dev_id", irq);
		return (NULL);
	}
	lkpi_irq_hw_disable(v);
	v->lv_requested = false;
	name = v->lv_name;
	mutex_exit(&v->lv_lock);

	/* Outside lv_lock: removal may wait for a running handler. */
	(void) ddi_intr_remove_handler(v->lv_h);
	synchronize_irq(irq);

	mutex_enter(&v->lv_lock);
	v->lv_handler = NULL;
	v->lv_thread_fn = NULL;
	v->lv_dev_id = NULL;
	v->lv_name = NULL;
	mutex_exit(&v->lv_lock);
	return (name);
}

void
synchronize_irq(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	v = lkpi_irq_get(irq);
	if (v == NULL)
		return;
	if (mutex_owned(&v->lv_run)) {
		WARN(1, "synchronize_irq: irq %u from its own handler", irq);
		return;
	}
	mutex_enter(&v->lv_run);
	mutex_exit(&v->lv_run);
}

void
synchronize_hardirq(unsigned int irq)
{
	synchronize_irq(irq);
}

void
disable_irq_nosync(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	v = lkpi_irq_get(irq);
	if (v == NULL)
		return;
	mutex_enter(&v->lv_lock);
	if (v->lv_depth++ == 0 && v->lv_requested)
		lkpi_irq_hw_disable(v);
	mutex_exit(&v->lv_lock);
}

void
disable_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	synchronize_irq(irq);
}

void
enable_irq(unsigned int irq)
{
	struct lkpi_irq_vec *v;

	v = lkpi_irq_get(irq);
	if (v == NULL)
		return;
	mutex_enter(&v->lv_lock);
	if (v->lv_depth == 0) {
		mutex_exit(&v->lv_lock);
		WARN(1, "enable_irq: irq %u not disabled", irq);
		return;
	}
	if (--v->lv_depth == 0 && v->lv_requested &&
	    lkpi_irq_hw_enable(v) != 0)
		cmn_err(CE_WARN, "linuxkpi: cannot enable irq %u", irq);
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
		(mutex_destroy)(&v->lv_lock);
		(mutex_destroy)(&v->lv_run);
		kmem_free(v, sizeof (*v));
	}
	for (i = 0; i < li->li_count; i++)
		(void) ddi_intr_free(li->li_h[i]);
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
		v->lv_block = (type == DDI_INTR_TYPE_MSI &&
		    (cap & DDI_INTR_FLAG_BLOCK) != 0);
		(mutex_init)(&v->lv_run, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(pri));
		(mutex_init)(&v->lv_lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(pri));
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
