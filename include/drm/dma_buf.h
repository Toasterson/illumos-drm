/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * dma-bufs (PRIME) in the illumos-drm facade.
 *
 * Origin: struct dma_buf and struct dma_buf_attachment keep the names and
 * relative order of the fields of Linux v7.2's include/linux/dma-buf.h
 * that drivers read; the functions mirror that header's and
 * include/drm/drm_prime.h's. dma-buf.h is GPL-2.0; this file is a
 * reimplementation for illumos from the documented interface, not a copy,
 * and is MIT-licensed (decision D7 of illumos-drm, 2026-10-01).
 */

#ifndef _DMA_BUF_H
#define	_DMA_BUF_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * A dma-buf: a GEM object shared through a file descriptor. In facade
 * version 0 every dma-buf is exported by the core (PRIME_HANDLE_TO_FD,
 * drm_gem_prime_export()), and a struct dma_buf is the core's handle for
 * one: size, resv (the exporting object's reservation), priv (the
 * exporting struct drm_gem_object) and exp_name are set; file, ops and
 * attachments are not used. Two handles for the same buffer may be
 * different structures; compare priv.
 */
struct dma_buf {
	size_t size;
	DRM_FACADE_FILE_T *file;
	struct list_head attachments;
	const void *ops;
	const char *exp_name;
	const char *name;
	void *priv;
	struct dma_resv *resv;
	/* private: the core's state */
	uint64_t __core[2];
};

/*
 * The import of a dma-buf by a device: obj->import_attach of an object
 * that drm_gem_prime_import() made from another device's dma-buf. dmabuf
 * is the buffer (the importing object holds it), dev the importing
 * device's node.
 */
struct dma_buf_attachment {
	struct dma_buf *dmabuf;
	dev_info_t *dev;
	struct list_head node;
	void *priv;
};

/*
 * The dma-buf of an object, with a new reference: its export, as Linux's
 * drm_gem_prime_export(). `flags` are the PRIME flags (DRM_CLOEXEC,
 * DRM_RDWR). An object made by drm_gem_prime_import() of another device's
 * dma-buf exports that buffer again (priv is the original exporter's
 * object), as Linux re-exports an import's dma-buf. Returns the dma-buf or
 * ERR_PTR(-errno). Thread context.
 */
extern struct dma_buf *drm_gem_prime_export(struct drm_gem_object *obj,
    int flags);

/*
 * An object of `dev` for a dma-buf: the exporting object itself if dev
 * exported it, otherwise a new object of dev that maps the exporter's
 * memory and keeps the dma-buf (import_attach). Returns it with a new
 * reference, or ERR_PTR(-errno).
 */
extern struct drm_gem_object *drm_gem_prime_import(struct drm_device *dev,
    struct dma_buf *dma_buf);

/*
 * The dma-buf of file descriptor `fd` of the calling process, with a new
 * reference, or ERR_PTR(-EBADF) / ERR_PTR(-EINVAL) for a descriptor that
 * is not a dma-buf. Only inside an ioctl handler.
 */
extern struct dma_buf *dma_buf_get(int fd);

/*
 * A new file descriptor of the calling process for the dma-buf (flags:
 * O_CLOEXEC, and DRM_RDWR for writable mappings), or a negative errno.
 * As on Linux, a descriptor takes over the caller's reference: on success
 * the caller must not dma_buf_put() it; on failure the caller still owns
 * it. Only inside an ioctl handler.
 */
extern int dma_buf_fd(struct dma_buf *dmabuf, int flags);

extern void get_dma_buf(struct dma_buf *dmabuf);
/*
 * Drops a reference; the contexts of drm_gem_object_put() (a thread that
 * may block, or an interrupt handler).
 */
extern void dma_buf_put(struct dma_buf *dmabuf);

#ifdef	__cplusplus
}
#endif

#endif	/* _DMA_BUF_H */
