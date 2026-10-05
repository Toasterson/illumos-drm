/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * ioremap() over ddi_regs_map_setup(9F) (the interface of Linux's
 * <linux/io.h>; independent implementation).
 *
 * The physical range must lie in a BAR of an attached layer PCI device
 * (lkpi_pci_phys_lookup(), src/lkpi_pci.c), which gives the node, the
 * "reg" index and the offset into it; the BAR is mapped with
 * DDI_STRICTORDER_ACC (ioremap(), ioremap_uc()) or DDI_MERGING_OK_ACC
 * (ioremap_wc(), write-combining). Each mapping's access handle is
 * recorded by its address for iounmap().
 *
 * arch_phys_wc_add() and arch_io_reserve_memtype_wc() have nothing to do:
 * write-combining comes from the mapping's attribute (the HAT programs
 * PAT for each mapping), not from MTRRs or a memory-type reservation.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/bug.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

struct lkpi_iomap {
	struct list_head	im_link;
	caddr_t			im_kva;
	ddi_acc_handle_t	im_handle;
};

/* A driver has a handful of register mappings: a list is enough. */
static kmutex_t lkpi_io_lock;
static struct list_head lkpi_iomaps;
static bool lkpi_io_ready;

static void __iomem *
lkpi_ioremap(resource_size_t phys, unsigned long size, uchar_t order)
{
	ddi_device_acc_attr_t acc = {
		.devacc_attr_version = DDI_DEVICE_ATTR_V0,
		.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
		.devacc_attr_dataorder = order
	};
	struct lkpi_iomap *im;
	dev_info_t *dip;
	uint_t regnum;
	off_t off;

	if (!lkpi_io_ready || size == 0)
		return (NULL);
	if (lkpi_pci_phys_lookup(phys, size, &dip, &regnum, &off) != 0)
		return (NULL);
	im = kmem_zalloc(sizeof (*im), KM_SLEEP);
	if (ddi_regs_map_setup(dip, regnum, &im->im_kva, (offset_t)off,
	    (offset_t)size, &acc, &im->im_handle) != DDI_SUCCESS) {
		kmem_free(im, sizeof (*im));
		return (NULL);
	}
	mutex_enter(&lkpi_io_lock);
	list_add(&im->im_link, &lkpi_iomaps);
	mutex_exit(&lkpi_io_lock);
	return (im->im_kva);
}

void __iomem *
ioremap(resource_size_t offset, unsigned long size)
{
	return (lkpi_ioremap(offset, size, DDI_STRICTORDER_ACC));
}

void __iomem *
ioremap_uc(resource_size_t offset, unsigned long size)
{
	return (lkpi_ioremap(offset, size, DDI_STRICTORDER_ACC));
}

void __iomem *
ioremap_wc(resource_size_t offset, unsigned long size)
{
	return (lkpi_ioremap(offset, size, DDI_MERGING_OK_ACC));
}

void
iounmap(volatile void __iomem *addr)
{
	struct lkpi_iomap *im, *found = NULL;

	if (addr == NULL || !lkpi_io_ready)
		return;
	mutex_enter(&lkpi_io_lock);
	list_for_each_entry(im, &lkpi_iomaps, im_link) {
		if (im->im_kva == (caddr_t)(uintptr_t)addr) {
			list_del(&im->im_link);
			found = im;
			break;
		}
	}
	mutex_exit(&lkpi_io_lock);
	if (found == NULL) {
		WARN(1, "iounmap: %p is not an ioremap() address",
		    (void *)(uintptr_t)addr);
		return;
	}
	ddi_regs_map_free(&found->im_handle);
	kmem_free(found, sizeof (*found));
}

int
arch_phys_wc_add(unsigned long base, unsigned long size)
{
	return (0);
}

void
arch_phys_wc_del(int handle)
{
}

int
arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size)
{
	return (0);
}

void
arch_io_free_memtype_wc(resource_size_t start, resource_size_t size)
{
}

/* Called by lkpi_page_init()/lkpi_page_fini(). */
void
lkpi_io_init(void)
{
	(mutex_init)(&lkpi_io_lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&lkpi_iomaps);
	lkpi_io_ready = true;
}

void
lkpi_io_fini(void)
{
	struct lkpi_iomap *im, *tmp;
	unsigned long n = 0;

	lkpi_io_ready = false;
	list_for_each_entry_safe(im, tmp, &lkpi_iomaps, im_link) {
		list_del(&im->im_link);
		ddi_regs_map_free(&im->im_handle);
		kmem_free(im, sizeof (*im));
		n++;
	}
	if (n != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu ioremap() mappings left", n);
	(mutex_destroy)(&lkpi_io_lock);
}
