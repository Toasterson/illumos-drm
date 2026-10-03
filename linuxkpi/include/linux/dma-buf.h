/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's dma-buf: the DRM facade's (<drm/dma_buf.h>, misc/drm_core),
 * which creates and imports dma-bufs for GEM objects itself
 * (drm_gem_prime_export/import), plus the Linux attachment interface.
 * Attaching to and mapping another driver's dma-buf (dma_buf_attach(),
 * dma_buf_map_attachment()) is not supported by the core yet: they fail
 * with -ENODEV (G5 open issue); a driver's own dma_buf_ops are kept but
 * the core does not call them.
 */
#ifndef _LINUX_DMA_BUF_H
#define	_LINUX_DMA_BUF_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/dma-resv.h>
#include <linux/dma-fence.h>
#include <linux/fs.h>
#include <drm/dma_buf.h>

struct dma_buf_attachment_ops {
	bool allow_peer2peer;
	void (*move_notify)(struct dma_buf_attachment *attach);
};

struct dma_buf_ops {
	int (*attach)(struct dma_buf *, struct dma_buf_attachment *);
	void (*detach)(struct dma_buf *, struct dma_buf_attachment *);
	int (*pin)(struct dma_buf_attachment *attach);
	void (*unpin)(struct dma_buf_attachment *attach);
	struct sg_table *(*map_dma_buf)(struct dma_buf_attachment *,
	    enum dma_data_direction);
	void (*unmap_dma_buf)(struct dma_buf_attachment *, struct sg_table *,
	    enum dma_data_direction);
	void (*release)(struct dma_buf *);
	int (*begin_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*end_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*mmap)(struct dma_buf *, struct vm_area_struct *vma);
	int (*vmap)(struct dma_buf *dmabuf, struct iosys_map *map);
	void (*vunmap)(struct dma_buf *dmabuf, struct iosys_map *map);
};

struct dma_buf_export_info {
	const char *exp_name;
	struct lx_module *owner;
	const struct dma_buf_ops *ops;
	size_t size;
	int flags;
	struct dma_resv *resv;
	void *priv;
};

#define	DEFINE_DMA_BUF_EXPORT_INFO(name) \
	struct dma_buf_export_info name = { .exp_name = KBUILD_MODNAME, .owner = THIS_MODULE }

extern struct dma_buf *dma_buf_export(const struct dma_buf_export_info *exp_info);
extern struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
    struct device *dev);
extern struct dma_buf_attachment *dma_buf_dynamic_attach(struct dma_buf *dmabuf,
    struct device *dev, const struct dma_buf_attachment_ops *importer_ops,
    void *importer_priv);
extern void dma_buf_detach(struct dma_buf *dmabuf,
    struct dma_buf_attachment *attach);
extern struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *,
    enum dma_data_direction);
extern struct sg_table *dma_buf_map_attachment_unlocked(
    struct dma_buf_attachment *, enum dma_data_direction);
extern void dma_buf_unmap_attachment(struct dma_buf_attachment *,
    struct sg_table *, enum dma_data_direction);
extern void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *,
    struct sg_table *, enum dma_data_direction);
extern int dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map);
extern void dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map);
extern int dma_buf_pin(struct dma_buf_attachment *attach);
extern void dma_buf_unpin(struct dma_buf_attachment *attach);

#endif
