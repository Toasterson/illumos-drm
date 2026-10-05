/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's DMA API over illumos DDI DMA (include/linux/dma-mapping.h,
 * GPL-2.0; reimplemented in src/lkpi_dma.c).
 *
 * A device's DMA attributes come from dma_set_mask() (the address limit)
 * and the layer's defaults (byte granularity, unlimited
 * scatter/gather list, 4 GiB segments). dma_map_sg() binds runs of
 * virtually contiguous entries with ddi_dma_addr_bind_handle(9F) and
 * spreads the cookies over the entries; an entry must not straddle two
 * cookies, which holds for page-sized entries (dma_max_mapping_size()
 * reports PAGE_SIZE, which i915 uses as its segment size) and for any
 * physically contiguous run. The handles are kept with the table and
 * released by dma_unmap_sg(). dma_alloc_coherent() is ddi_dma_mem_alloc(9F)
 * with DDI_DMA_CONSISTENT and one cookie. dma_map_page()/dma_map_single()
 * bind one range and must give one cookie. Streaming mappings of memory
 * outside the device's address range fail instead of using an illumos
 * copy buffer (i915 does not sync its objects; see src/lkpi_dma.c).
 * Mapping errors return DMA_MAPPING_ERROR / 0 entries, as on Linux.
 */
#ifndef _LINUX_DMA_MAPPING_H
#define	_LINUX_DMA_MAPPING_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/dma-direction.h>
#include <linux/err.h>

#define	DMA_MAPPING_ERROR	(~(dma_addr_t)0)
#define	DMA_BIT_MASK(n)		(((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define	DMA_ATTR_WEAK_ORDERING		(1UL << 1)
#define	DMA_ATTR_WRITE_COMBINE		(1UL << 2)
#define	DMA_ATTR_NO_KERNEL_MAPPING	(1UL << 4)
#define	DMA_ATTR_SKIP_CPU_SYNC		(1UL << 5)
#define	DMA_ATTR_FORCE_CONTIGUOUS	(1UL << 6)
#define	DMA_ATTR_NO_WARN		(1UL << 8)

extern int dma_set_mask(struct device *dev, u64 mask);
extern int dma_set_coherent_mask(struct device *dev, u64 mask);
extern int dma_set_mask_and_coherent(struct device *dev, u64 mask);
extern u64 dma_get_mask(struct device *dev);
extern size_t dma_max_mapping_size(struct device *dev);
extern int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg,
    int nents, enum dma_data_direction dir, unsigned long attrs);
extern void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg,
    int nents, enum dma_data_direction dir, unsigned long attrs);
extern int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
    enum dma_data_direction dir, unsigned long attrs);
extern void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
    enum dma_data_direction dir, unsigned long attrs);
extern dma_addr_t dma_map_page_attrs(struct device *dev, struct lx_page *page,
    size_t offset, size_t size, enum dma_data_direction dir,
    unsigned long attrs);
extern void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr,
    size_t size, enum dma_data_direction dir, unsigned long attrs);
extern dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys,
    size_t size, enum dma_data_direction dir, unsigned long attrs);
extern void dma_unmap_resource(struct device *dev, dma_addr_t addr,
    size_t size, enum dma_data_direction dir, unsigned long attrs);
extern void *dma_alloc_attrs(struct device *dev, size_t size,
    dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs);
extern void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
    dma_addr_t dma_handle, unsigned long attrs);
extern void dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
    int nelems, enum dma_data_direction dir);
extern void dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
    int nelems, enum dma_data_direction dir);
extern void dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr,
    size_t size, enum dma_data_direction dir);
extern void dma_sync_single_for_device(struct device *dev, dma_addr_t addr,
    size_t size, enum dma_data_direction dir);

#define	dma_map_sg(d, s, n, r)		dma_map_sg_attrs(d, s, n, r, 0)
#define	dma_unmap_sg(d, s, n, r)	dma_unmap_sg_attrs(d, s, n, r, 0)
#define	dma_map_page(d, p, o, s, r)	dma_map_page_attrs(d, p, o, s, r, 0)
#define	dma_unmap_page(d, a, s, r)	dma_unmap_page_attrs(d, a, s, r, 0)
#define	dma_map_single(d, a, s, r) \
	dma_map_page_attrs(d, virt_to_page(a), offset_in_page(a), s, r, 0)
#define	dma_unmap_single(d, a, s, r)	dma_unmap_page_attrs(d, a, s, r, 0)
#define	dma_map_single_attrs(d, a, s, r, at) \
	dma_map_page_attrs(d, virt_to_page(a), offset_in_page(a), s, r, at)
#define	dma_unmap_single_attrs(d, a, s, r, at)	dma_unmap_page_attrs(d, a, s, r, at)
#define	dma_alloc_coherent(d, s, h, g)	dma_alloc_attrs(d, s, h, g, 0)
#define	dma_free_coherent(d, s, c, h)	dma_free_attrs(d, s, c, h, 0)
#define	dma_alloc_wc(d, s, h, g)	dma_alloc_attrs(d, s, h, g, DMA_ATTR_WRITE_COMBINE)
#define	dma_free_wc(d, s, c, h)		dma_free_attrs(d, s, c, h, DMA_ATTR_WRITE_COMBINE)
#define	dma_mapping_error(d, a)		((a) == DMA_MAPPING_ERROR ? -ENOMEM : 0)
#define	dma_sync_sgtable_for_cpu(d, t, r) \
	dma_sync_sg_for_cpu(d, (t)->sgl, (t)->orig_nents, r)
#define	dma_sync_sgtable_for_device(d, t, r) \
	dma_sync_sg_for_device(d, (t)->sgl, (t)->orig_nents, r)
#define	dma_get_max_seg_size(d)		((unsigned int)PAGE_SIZE)
#define	dma_set_max_seg_size(d, s)	0
#define	dma_set_seg_boundary(d, m)	0
#define	dma_addressing_limited(d)	false
#define	dma_need_sync(d, a)		false
#define	dma_is_dma_mapped(d)		true

#endif
