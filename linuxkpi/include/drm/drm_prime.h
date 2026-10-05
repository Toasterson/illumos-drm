/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's PRIME helpers. PRIME (export/import of GEM objects as dma-bufs)
 * is the DRM core's: drm_gem_prime_export()/import() are the facade's. The
 * Linux helpers for drivers that build their own dma-buf ops
 * (drm_gem_dmabuf_export(), drm_gem_map_dma_buf(), ...) are not supported:
 * they fail (ERR_PTR(-ENODEV), NULL) and the release helpers do nothing
 * (src/lkpi_drm.c).
 */
#ifndef _LKPI_DRM_PRIME_H
#define	_LKPI_DRM_PRIME_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <drm/drm_gem.h>

extern struct dma_buf *drm_gem_dmabuf_export(struct drm_device *dev,
    struct dma_buf_export_info *exp_info);
extern void drm_gem_dmabuf_release(struct dma_buf *dma_buf);
extern int drm_gem_prime_mmap(struct drm_gem_object *obj,
    struct vm_area_struct *vma);
extern struct sg_table *drm_gem_map_dma_buf(struct dma_buf_attachment *attach,
    enum dma_data_direction dir);
extern void drm_gem_unmap_dma_buf(struct dma_buf_attachment *attach,
    struct sg_table *sgt, enum dma_data_direction dir);
extern void drm_prime_gem_destroy(struct drm_gem_object *obj,
    struct sg_table *sg);
extern int drm_gem_map_attach(struct dma_buf *dma_buf,
    struct dma_buf_attachment *attach);
extern void drm_gem_map_detach(struct dma_buf *dma_buf,
    struct dma_buf_attachment *attach);

#endif
