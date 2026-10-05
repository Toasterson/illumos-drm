/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_syncobj.h>: the facade's sync objects (include_next)
 * plus drm_syncobj_create()/drm_syncobj_get()/drm_syncobj_fence_get(),
 * which ask the core (sync objects are the core's; their structure is
 * opaque to C).
 */
#ifndef _LKPI_DRM_SYNCOBJ_H
#define	_LKPI_DRM_SYNCOBJ_H

#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <drm/drm_file.h>
#include_next <drm/drm_syncobj.h>

#define	DRM_SYNCOBJ_CREATE_SIGNALED	(1 << 0)

extern int drm_syncobj_create(struct drm_syncobj **out_syncobj,
    uint32_t flags, struct dma_fence *fence);
extern void drm_syncobj_get(struct drm_syncobj *obj);
extern struct dma_fence *drm_syncobj_fence_get(struct drm_syncobj *syncobj);
extern int drm_syncobj_get_handle(struct drm_file *file_private,
    struct drm_syncobj *syncobj, u32 *handle);

#endif
