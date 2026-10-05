/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's DMA API over illumos DDI DMA (the interface of Linux's
 * <linux/dma-mapping.h>; independent implementation, see the header for
 * the contract).
 *
 * A layer device carries its ddi_dma_attr_t (lkpi_dev_dma_attr(),
 * initialised by the PCI glue; dma_set_mask() sets dma_attr_addr_hi).
 *
 * dma_map_sg(): consecutive entries whose kernel addresses are contiguous
 * form a run; each run is bound with ddi_dma_addr_bind_handle(9F) and its
 * cookies are spread over the run's entries in order. An entry must lie
 * within one cookie (page-sized entries always do, being physically
 * contiguous); otherwise the whole mapping is undone and 0 returned. No
 * entries are merged, so the mapped count is the entry count. The
 * handles are recorded in an index keyed by the list's first entry, which
 * dma_unmap_sg(), the dma_sync_sg_*() calls and sg_free_table() (which
 * drops a mapping a driver forgot, with a warning) look up; for
 * dma_map_sgtable() the table's lkpi_dma points at the record as well.
 * Mapping does not sleep (DDI_DMA_DONTWAIT, KM_NOSLEEP), as Linux's may
 * be called in atomic context: it fails instead, and i915 then shrinks
 * and retries.
 *
 * dma_map_page()/dma_map_single() bind one range that must give one
 * cookie (a physically contiguous range); dma_map_resource() first maps
 * the physical range into the kernel (vmap_pfn(), uncached) and binds
 * that. dma_alloc_coherent() is ddi_dma_mem_alloc(9F) with
 * DDI_DMA_CONSISTENT and a one-cookie attribute (physically contiguous,
 * page-aligned, below the coherent mask), zeroed; dma_alloc_wc() asks for
 * IOMEM_DATA_UC_WR_COMBINE memory. dma_sync_*() are ddi_dma_sync(9F).
 */

#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

/* A dma_map_sg(): the handles of its runs. */
struct lkpi_dma_sg {
	avl_node_t		ds_node;
	const struct scatterlist *ds_sgl;
	uint64_t		ds_seq;
	uint_t			ds_cap;
	uint_t			ds_nhandles;
	ddi_dma_handle_t	ds_handles[];
};

/* A dma_map_page() or dma_map_resource(). */
struct lkpi_dma_one {
	avl_node_t		do_node;
	dma_addr_t		do_addr;
	uint64_t		do_seq;
	size_t			do_size;
	ddi_dma_handle_t	do_handle;
	void			*do_va;		/* dma_map_resource()'s mapping */
};

/* A dma_alloc_attrs(). */
struct lkpi_dma_coherent {
	avl_node_t		dc_node;
	caddr_t			dc_kva;
	size_t			dc_size;
	ddi_dma_handle_t	dc_handle;
	ddi_acc_handle_t	dc_acc;
};

static kmutex_t lkpi_dma_lock;
static avl_tree_t lkpi_dma_sgs;
static avl_tree_t lkpi_dma_ones;
static avl_tree_t lkpi_dma_coherents;
static uint64_t lkpi_dma_seq;
static bool lkpi_dma_ready;

static int
lkpi_dma_sg_cmp(const void *a, const void *b)
{
	const struct lkpi_dma_sg *x = a, *y = b;

	if ((uintptr_t)x->ds_sgl != (uintptr_t)y->ds_sgl)
		return ((uintptr_t)x->ds_sgl < (uintptr_t)y->ds_sgl ? -1 : 1);
	if (x->ds_seq != y->ds_seq)
		return (x->ds_seq < y->ds_seq ? -1 : 1);
	return (0);
}

static int
lkpi_dma_one_cmp(const void *a, const void *b)
{
	const struct lkpi_dma_one *x = a, *y = b;

	if (x->do_addr != y->do_addr)
		return (x->do_addr < y->do_addr ? -1 : 1);
	if (x->do_seq != y->do_seq)
		return (x->do_seq < y->do_seq ? -1 : 1);
	return (0);
}

static int
lkpi_dma_coherent_cmp(const void *a, const void *b)
{
	uintptr_t x = (uintptr_t)((const struct lkpi_dma_coherent *)a)->dc_kva;
	uintptr_t y = (uintptr_t)((const struct lkpi_dma_coherent *)b)->dc_kva;

	return (x < y ? -1 : (x > y ? 1 : 0));
}

/* The device's node and DMA attributes, or NULL for a device without DMA. */
static dev_info_t *
lkpi_dma_dev(struct device *dev, ddi_dma_attr_t **attrp)
{
	ddi_dma_attr_t *attr;
	dev_info_t *dip;

	if (dev == NULL || (attr = lkpi_dev_dma_attr(dev)) == NULL ||
	    (dip = lkpi_dev_dip(dev)) == NULL) {
		WARN_ONCE(1, "DMA on a device without DMA attributes");
		return (NULL);
	}
	*attrp = attr;
	return (dip);
}

static uint_t
lkpi_dma_dirflags(enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_TO_DEVICE:
		return (DDI_DMA_WRITE);
	case DMA_FROM_DEVICE:
		return (DDI_DMA_READ);
	default:
		return (DDI_DMA_RDWR);
	}
}

static uint_t
lkpi_dma_syncflag(bool for_cpu)
{
	return (for_cpu ? DDI_DMA_SYNC_FORCPU : DDI_DMA_SYNC_FORDEV);
}

/* Binds [kva, kva + len) without sleeping. */
static int
lkpi_dma_bind(dev_info_t *dip, const ddi_dma_attr_t *attr, caddr_t kva,
    size_t len, uint_t flags, ddi_dma_handle_t *hp, uint_t *ccountp)
{
	ddi_dma_handle_t h;
	ddi_dma_cookie_t c;

	if (ddi_dma_alloc_handle(dip, attr, DDI_DMA_DONTWAIT, NULL, &h) !=
	    DDI_SUCCESS)
		return (-ENOMEM);
	if (ddi_dma_addr_bind_handle(h, NULL, kva, len, flags,
	    DDI_DMA_DONTWAIT, NULL, &c, ccountp) != DDI_DMA_MAPPED) {
		ddi_dma_free_handle(&h);
		return (-ENOMEM);
	}
	*hp = h;
	return (0);
}

static void
lkpi_dma_unbind(ddi_dma_handle_t *hp)
{
	(void) ddi_dma_unbind_handle(*hp);
	ddi_dma_free_handle(hp);
}

/* --- scatterlists ------------------------------------------------------ */

static caddr_t
lkpi_sg_kva(struct scatterlist *sg)
{
	struct lx_page *page;

	if (sg->lkpi_buf != NULL)
		return ((caddr_t)sg->lkpi_buf + sg->offset);
	if ((page = sg_page(sg)) == NULL)
		return (NULL);
	return ((caddr_t)page->lkpi_kva + sg->offset);
}

static void
lkpi_dma_sg_free(struct lkpi_dma_sg *ds)
{
	uint_t i;

	for (i = 0; i < ds->ds_nhandles; i++)
		lkpi_dma_unbind(&ds->ds_handles[i]);
	kmem_free(ds, sizeof (*ds) + ds->ds_cap * sizeof (ddi_dma_handle_t));
}

/* Removes and returns the newest mapping of the list, or NULL. */
static struct lkpi_dma_sg *
lkpi_dma_sg_take(const struct scatterlist *sgl)
{
	struct lkpi_dma_sg key, *ds;
	avl_index_t where;

	if (!lkpi_dma_ready || sgl == NULL)
		return (NULL);
	key.ds_sgl = sgl;
	key.ds_seq = UINT64_MAX;
	mutex_enter(&lkpi_dma_lock);
	(void) avl_find(&lkpi_dma_sgs, &key, &where);
	ds = avl_nearest(&lkpi_dma_sgs, where, AVL_BEFORE);
	if (ds != NULL && ds->ds_sgl == sgl)
		avl_remove(&lkpi_dma_sgs, ds);
	else
		ds = NULL;
	mutex_exit(&lkpi_dma_lock);
	return (ds);
}

static struct lkpi_dma_sg *
lkpi_dma_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
    enum dma_data_direction dir)
{
	struct scatterlist *sg, *s, *next, *first;
	const ddi_dma_cookie_t *c;
	ddi_dma_attr_t *attr;
	struct lkpi_dma_sg *ds;
	ddi_dma_handle_t h;
	dev_info_t *dip;
	caddr_t kva, prev_end = NULL;
	uint64_t caddr, cleft;
	uint_t flags, nruns = 0, cc;
	size_t len;
	int i, k, cnt;

	if (sgl == NULL || nents <= 0 || (dip = lkpi_dma_dev(dev, &attr)) ==
	    NULL)
		return (NULL);
	flags = lkpi_dma_dirflags(dir) | DDI_DMA_STREAMING;

	for_each_sg(sgl, sg, nents, i) {
		if (sg->length == 0)
			continue;
		if ((kva = lkpi_sg_kva(sg)) == NULL) {
			WARN(1, "dma_map_sg: entry %d has no memory", i);
			return (NULL);
		}
		if (kva != prev_end)
			nruns++;
		prev_end = kva + sg->length;
	}
	ds = kmem_zalloc(sizeof (*ds) + nruns * sizeof (ddi_dma_handle_t),
	    KM_NOSLEEP);
	if (ds == NULL)
		return (NULL);
	ds->ds_sgl = sgl;
	ds->ds_cap = nruns;

	sg = sgl;
	i = 0;
	while (i < nents && sg != NULL) {
		if (sg->length == 0) {
			sg->dma_address = 0;
			sg->dma_length = 0;
			sg = (i + 1 < nents) ? sg_next(sg) : NULL;
			i++;
			continue;
		}
		/* The run starting here. */
		first = sg;
		kva = lkpi_sg_kva(sg);
		len = 0;
		cnt = 0;
		for (s = sg; ; s = next) {
			len += s->length;
			cnt++;
			next = (i + cnt < nents) ? sg_next(s) : NULL;
			if (next == NULL || next->length == 0 ||
			    lkpi_sg_kva(next) != kva + len)
				break;
		}
		if (lkpi_dma_bind(dip, attr, kva, len, flags, &h, &cc) != 0)
			goto fail;
		ds->ds_handles[ds->ds_nhandles++] = h;

		c = ddi_dma_cookie_iter(h, NULL);
		caddr = c->dmac_laddress;
		cleft = c->dmac_size;
		for (k = 0, s = first; k < cnt; k++, s = sg_next(s)) {
			while (cleft == 0 && c != NULL) {
				if ((c = ddi_dma_cookie_iter(h, c)) != NULL) {
					caddr = c->dmac_laddress;
					cleft = c->dmac_size;
				}
			}
			if (c == NULL || s->length > cleft) {
				WARN_ONCE(1, "dma_map_sg: an entry of %u bytes "
				    "spans two DMA cookies", s->length);
				goto fail;
			}
			s->dma_address = caddr;
			s->dma_length = s->length;
			caddr += s->length;
			cleft -= s->length;
		}
		sg = next;
		i += cnt;
	}

	mutex_enter(&lkpi_dma_lock);
	ds->ds_seq = ++lkpi_dma_seq;
	avl_add(&lkpi_dma_sgs, ds);
	mutex_exit(&lkpi_dma_lock);
	return (ds);
fail:
	lkpi_dma_sg_free(ds);
	return (NULL);
}

int
dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
    enum dma_data_direction dir, unsigned long attrs)
{
	return (lkpi_dma_map_sg(dev, sg, nents, dir) != NULL ? nents : 0);
}

void
dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
    enum dma_data_direction dir, unsigned long attrs)
{
	struct lkpi_dma_sg *ds = lkpi_dma_sg_take(sg);

	if (ds == NULL) {
		WARN_ONCE(1, "dma_unmap_sg: %p is not mapped", sg);
		return;
	}
	lkpi_dma_sg_free(ds);
}

int
dma_map_sgtable(struct device *dev, struct sg_table *sgt,
    enum dma_data_direction dir, unsigned long attrs)
{
	struct lkpi_dma_sg *ds;

	if (sgt == NULL || sgt->orig_nents == 0)
		return (-EINVAL);
	ds = lkpi_dma_map_sg(dev, sgt->sgl, (int)sgt->orig_nents, dir);
	if (ds == NULL)
		return (-ENOMEM);
	sgt->nents = sgt->orig_nents;
	sgt->lkpi_dma = ds;
	return (0);
}

void
dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
    enum dma_data_direction dir, unsigned long attrs)
{
	if (sgt == NULL)
		return;
	dma_unmap_sg_attrs(dev, sgt->sgl, (int)sgt->orig_nents, dir, attrs);
	sgt->lkpi_dma = NULL;
}

void
lkpi_dma_sgl_release(struct scatterlist *sgl)
{
	struct lkpi_dma_sg *ds;

	while ((ds = lkpi_dma_sg_take(sgl)) != NULL) {
		WARN_ONCE(1, "sg_free_table: %p is still DMA-mapped", sgl);
		lkpi_dma_sg_free(ds);
	}
}

static void
lkpi_dma_sync_sg(struct scatterlist *sg, bool for_cpu)
{
	struct lkpi_dma_sg key, *ds;
	avl_index_t where;
	uint_t i;

	if (!lkpi_dma_ready || sg == NULL)
		return;
	key.ds_sgl = sg;
	key.ds_seq = UINT64_MAX;
	mutex_enter(&lkpi_dma_lock);
	(void) avl_find(&lkpi_dma_sgs, &key, &where);
	ds = avl_nearest(&lkpi_dma_sgs, where, AVL_BEFORE);
	if (ds != NULL && ds->ds_sgl == sg) {
		for (i = 0; i < ds->ds_nhandles; i++)
			(void) ddi_dma_sync(ds->ds_handles[i], 0, 0,
			    lkpi_dma_syncflag(for_cpu));
	}
	mutex_exit(&lkpi_dma_lock);
}

void
dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nelems,
    enum dma_data_direction dir)
{
	lkpi_dma_sync_sg(sg, true);
}

void
dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
    int nelems, enum dma_data_direction dir)
{
	lkpi_dma_sync_sg(sg, false);
}

/* --- single ranges ----------------------------------------------------- */

static dma_addr_t
lkpi_dma_map_one(struct device *dev, caddr_t kva, size_t size,
    enum dma_data_direction dir, void *va)
{
	struct lkpi_dma_one *one;
	ddi_dma_attr_t *attr;
	ddi_dma_handle_t h;
	dev_info_t *dip;
	uint_t cc;

	if (size == 0 || (dip = lkpi_dma_dev(dev, &attr)) == NULL)
		return (DMA_MAPPING_ERROR);
	if (lkpi_dma_bind(dip, attr, kva, size,
	    lkpi_dma_dirflags(dir) | DDI_DMA_STREAMING, &h, &cc) != 0)
		return (DMA_MAPPING_ERROR);
	if (cc != 1) {
		/* Not physically contiguous: Linux's caller expects one address. */
		lkpi_dma_unbind(&h);
		return (DMA_MAPPING_ERROR);
	}
	if ((one = kmem_zalloc(sizeof (*one), KM_NOSLEEP)) == NULL) {
		lkpi_dma_unbind(&h);
		return (DMA_MAPPING_ERROR);
	}
	one->do_addr = ddi_dma_cookie_one(h)->dmac_laddress;
	one->do_size = size;
	one->do_handle = h;
	one->do_va = va;
	mutex_enter(&lkpi_dma_lock);
	one->do_seq = ++lkpi_dma_seq;
	avl_add(&lkpi_dma_ones, one);
	mutex_exit(&lkpi_dma_lock);
	return (one->do_addr);
}

/* The newest single mapping containing addr (exactly at it if `exact`). */
static struct lkpi_dma_one *
lkpi_dma_one_find(dma_addr_t addr, bool exact)
{
	struct lkpi_dma_one key, *one;
	avl_index_t where;

	ASSERT(MUTEX_HELD(&lkpi_dma_lock));
	key.do_addr = addr;
	key.do_seq = UINT64_MAX;
	(void) avl_find(&lkpi_dma_ones, &key, &where);
	one = avl_nearest(&lkpi_dma_ones, where, AVL_BEFORE);
	if (one == NULL || (exact && one->do_addr != addr) ||
	    addr >= one->do_addr + one->do_size)
		return (NULL);
	return (one);
}

dma_addr_t
dma_map_page_attrs(struct device *dev, struct lx_page *page, size_t offset,
    size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	if (page == NULL)
		return (DMA_MAPPING_ERROR);
	return (lkpi_dma_map_one(dev, (caddr_t)page->lkpi_kva + offset, size,
	    dir, NULL));
}

void
dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
	struct lkpi_dma_one *one = NULL;

	if (lkpi_dma_ready) {
		mutex_enter(&lkpi_dma_lock);
		if ((one = lkpi_dma_one_find(addr, true)) != NULL)
			avl_remove(&lkpi_dma_ones, one);
		mutex_exit(&lkpi_dma_lock);
	}
	if (one == NULL) {
		WARN_ONCE(1, "dma_unmap_page: %llx is not mapped",
		    (unsigned long long)addr);
		return;
	}
	lkpi_dma_unbind(&one->do_handle);
	if (one->do_va != NULL)
		vunmap(one->do_va);
	kmem_free(one, sizeof (*one));
}

dma_addr_t
dma_map_resource(struct device *dev, phys_addr_t phys, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
	size_t off = phys & (PAGE_SIZE - 1);
	unsigned long *pfns, n, i;
	dma_addr_t addr;
	caddr_t va;

	if (size == 0 || size > SIZE_MAX - PAGE_SIZE)
		return (DMA_MAPPING_ERROR);
	n = LKPI_BTOPR(off + size);
	if ((pfns = kmem_alloc(n * sizeof (*pfns), KM_NOSLEEP)) == NULL)
		return (DMA_MAPPING_ERROR);
	for (i = 0; i < n; i++)
		pfns[i] = (unsigned long)LKPI_BTOP(phys) + i;
	va = vmap_pfn(pfns, (unsigned int)n, PAGE_KERNEL_IO);
	kmem_free(pfns, n * sizeof (*pfns));
	if (va == NULL)
		return (DMA_MAPPING_ERROR);
	addr = lkpi_dma_map_one(dev, va + off, size, dir, va);
	if (addr == DMA_MAPPING_ERROR)
		vunmap(va);
	return (addr);
}

void
dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
	dma_unmap_page_attrs(dev, addr, size, dir, attrs);
}

static void
lkpi_dma_sync_one(dma_addr_t addr, size_t size, bool for_cpu)
{
	struct lkpi_dma_one *one;

	if (!lkpi_dma_ready)
		return;
	mutex_enter(&lkpi_dma_lock);
	if ((one = lkpi_dma_one_find(addr, false)) != NULL) {
		(void) ddi_dma_sync(one->do_handle,
		    (off_t)(addr - one->do_addr),
		    MIN(size, one->do_size - (size_t)(addr - one->do_addr)),
		    lkpi_dma_syncflag(for_cpu));
	}
	mutex_exit(&lkpi_dma_lock);
}

void
dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir)
{
	lkpi_dma_sync_one(addr, size, true);
}

void
dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir)
{
	lkpi_dma_sync_one(addr, size, false);
}

/* --- coherent memory --------------------------------------------------- */

static void
lkpi_dma_coherent_free(struct lkpi_dma_coherent *dc)
{
	lkpi_dma_unbind(&dc->dc_handle);
	ddi_dma_mem_free(&dc->dc_acc);
	kmem_free(dc, sizeof (*dc));
}

void *
dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
    gfp_t gfp, unsigned long attrs)
{
	ddi_device_acc_attr_t acc = {
		.devacc_attr_version = DDI_DEVICE_ATTR_V0,
		.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
		.devacc_attr_dataorder = DDI_STRICTORDER_ACC
	};
	struct lkpi_dma_coherent *dc;
	int (*wait)(caddr_t);
	ddi_dma_attr_t *attr, a;
	ddi_dma_cookie_t c;
	dev_info_t *dip;
	size_t real;
	uint_t cc, xfer;

	if (size == 0 || dma_handle == NULL ||
	    (dip = lkpi_dma_dev(dev, &attr)) == NULL)
		return (NULL);
	a = *attr;
	a.dma_attr_sgllen = 1;
	a.dma_attr_align = MAX(a.dma_attr_align, PAGE_SIZE);
	if (dev->coherent_dma_mask != 0)
		a.dma_attr_addr_hi = MIN(a.dma_attr_addr_hi,
		    dev->coherent_dma_mask);
	wait = gfpflags_allow_blocking(gfp) ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT;
	xfer = DDI_DMA_CONSISTENT;
	if (attrs & DMA_ATTR_WRITE_COMBINE)
		xfer |= IOMEM_DATA_UC_WR_COMBINE;

	if ((dc = kmem_zalloc(sizeof (*dc), lkpi_kmflag(gfp))) == NULL)
		return (NULL);
	if (ddi_dma_alloc_handle(dip, &a, wait, NULL, &dc->dc_handle) !=
	    DDI_SUCCESS)
		goto fail;
	if (ddi_dma_mem_alloc(dc->dc_handle, size, &acc, xfer, wait, NULL,
	    &dc->dc_kva, &real, &dc->dc_acc) != DDI_SUCCESS)
		goto fail_handle;
	if (ddi_dma_addr_bind_handle(dc->dc_handle, NULL, dc->dc_kva, real,
	    DDI_DMA_RDWR | DDI_DMA_CONSISTENT, wait, NULL, &c, &cc) !=
	    DDI_DMA_MAPPED)
		goto fail_mem;
	if (cc != 1) {
		(void) ddi_dma_unbind_handle(dc->dc_handle);
		goto fail_mem;
	}
	bzero(dc->dc_kva, real);
	dc->dc_size = real;

	mutex_enter(&lkpi_dma_lock);
	avl_add(&lkpi_dma_coherents, dc);
	mutex_exit(&lkpi_dma_lock);
	*dma_handle = c.dmac_laddress;
	return (dc->dc_kva);

fail_mem:
	ddi_dma_mem_free(&dc->dc_acc);
fail_handle:
	ddi_dma_free_handle(&dc->dc_handle);
fail:
	kmem_free(dc, sizeof (*dc));
	return (NULL);
}

void
dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
    dma_addr_t dma_handle, unsigned long attrs)
{
	struct lkpi_dma_coherent key, *dc = NULL;

	if (cpu_addr == NULL)
		return;
	if (lkpi_dma_ready) {
		key.dc_kva = cpu_addr;
		mutex_enter(&lkpi_dma_lock);
		if ((dc = avl_find(&lkpi_dma_coherents, &key, NULL)) != NULL)
			avl_remove(&lkpi_dma_coherents, dc);
		mutex_exit(&lkpi_dma_lock);
	}
	if (dc == NULL) {
		WARN(1, "dma_free_coherent: %p was not allocated", cpu_addr);
		return;
	}
	lkpi_dma_coherent_free(dc);
}

/* --- masks ------------------------------------------------------------- */

int
dma_set_mask(struct device *dev, u64 mask)
{
	ddi_dma_attr_t *attr;

	if (dev == NULL || mask == 0 || (attr = lkpi_dev_dma_attr(dev)) == NULL)
		return (-EIO);
	attr->dma_attr_addr_hi = mask;
	if (dev->dma_mask != NULL)
		*dev->dma_mask = mask;
	return (0);
}

int
dma_set_coherent_mask(struct device *dev, u64 mask)
{
	if (dev == NULL || mask == 0 || lkpi_dev_dma_attr(dev) == NULL)
		return (-EIO);
	dev->coherent_dma_mask = mask;
	return (0);
}

int
dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
	int err;

	if ((err = dma_set_mask(dev, mask)) != 0)
		return (err);
	return (dma_set_coherent_mask(dev, mask));
}

u64
dma_get_mask(struct device *dev)
{
	ddi_dma_attr_t *attr;

	if (dev == NULL)
		return (DMA_BIT_MASK(32));
	if (dev->dma_mask != NULL && *dev->dma_mask != 0)
		return (*dev->dma_mask);
	if ((attr = lkpi_dev_dma_attr(dev)) != NULL)
		return (attr->dma_attr_addr_hi);
	return (DMA_BIT_MASK(32));
}

/* i915 uses this as its scatterlist segment size: one page per entry. */
size_t
dma_max_mapping_size(struct device *dev)
{
	return (PAGE_SIZE);
}

/* --- module state (called by lkpi_page_init/fini) ---------------------- */

void
lkpi_dma_init(void)
{
	(mutex_init)(&lkpi_dma_lock, NULL, MUTEX_DRIVER, NULL);
	avl_create(&lkpi_dma_sgs, lkpi_dma_sg_cmp, sizeof (struct lkpi_dma_sg),
	    offsetof(struct lkpi_dma_sg, ds_node));
	avl_create(&lkpi_dma_ones, lkpi_dma_one_cmp,
	    sizeof (struct lkpi_dma_one), offsetof(struct lkpi_dma_one, do_node));
	avl_create(&lkpi_dma_coherents, lkpi_dma_coherent_cmp,
	    sizeof (struct lkpi_dma_coherent),
	    offsetof(struct lkpi_dma_coherent, dc_node));
	lkpi_dma_ready = true;
}

/* Releases what the driver left mapped (it has detached). */
void
lkpi_dma_fini(void)
{
	struct lkpi_dma_coherent *dc;
	struct lkpi_dma_one *one;
	struct lkpi_dma_sg *ds;
	unsigned long n = 0;

	lkpi_dma_ready = false;
	while ((ds = avl_first(&lkpi_dma_sgs)) != NULL) {
		avl_remove(&lkpi_dma_sgs, ds);
		lkpi_dma_sg_free(ds);
		n++;
	}
	while ((one = avl_first(&lkpi_dma_ones)) != NULL) {
		avl_remove(&lkpi_dma_ones, one);
		lkpi_dma_unbind(&one->do_handle);
		if (one->do_va != NULL)
			vunmap(one->do_va);
		kmem_free(one, sizeof (*one));
		n++;
	}
	while ((dc = avl_first(&lkpi_dma_coherents)) != NULL) {
		avl_remove(&lkpi_dma_coherents, dc);
		lkpi_dma_coherent_free(dc);
		n++;
	}
	if (n != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu DMA mappings left", n);
	avl_destroy(&lkpi_dma_sgs);
	avl_destroy(&lkpi_dma_ones);
	avl_destroy(&lkpi_dma_coherents);
	(mutex_destroy)(&lkpi_dma_lock);
}
