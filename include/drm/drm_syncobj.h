/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Sync objects and sync files in the illumos-drm facade.
 *
 * Origin: the functions mirror Linux v7.2's include/drm/drm_syncobj.h
 * (MIT) and the sync-file part of include/linux/sync_file.h (GPL-2.0,
 * reimplemented from the documented interface, not copied). struct
 * drm_syncobj is opaque in facade version 0. MIT-licensed (decision D7 of
 * illumos-drm, 2026-10-01).
 */

#ifndef _DRM_SYNCOBJ_H
#define	_DRM_SYNCOBJ_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* The sync object of a handle with a new reference, or NULL. */
extern struct drm_syncobj *drm_syncobj_find(struct drm_file *file_private,
    uint32_t handle);

/* Drops a reference (Linux's inline drm_syncobj_put()). */
extern void drm_syncobj_put(struct drm_syncobj *obj);

/*
 * Replaces the object's fence (NULL resets it). Thread context only.
 */
extern void drm_syncobj_replace_fence(struct drm_syncobj *syncobj,
    struct dma_fence *fence);

/*
 * Adds `fence` as timeline point `point`, consuming `chain` (from
 * dma_fence_chain_alloc()). A point not later than the last one is
 * added with a warning, as Linux does. Thread context only.
 */
extern void drm_syncobj_add_point(struct drm_syncobj *syncobj,
    struct dma_fence_chain *chain, struct dma_fence *fence, uint64_t point);

/*
 * The fence of a handle's sync object at `point` (0: the current fence)
 * with a new reference. `flags` takes DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT.
 * Returns 0, -ENOENT (no object, no fence, or a point not submitted yet),
 * -EINVAL.
 */
extern int drm_syncobj_find_fence(struct drm_file *file_private,
    uint32_t handle, uint64_t point, uint64_t flags,
    struct dma_fence **fence);

/*
 * Sync files. Linux's sync_file_create() followed by fd_install() is one
 * step on illumos: drm_sync_file_install() makes a sync file of `fence`,
 * named after the driver of the ioctl's device, and a descriptor of the
 * calling process for it (with O_CLOEXEC if `cloexec`). Only inside an ioctl handler. Returns 0 and the descriptor
 * in *fdp, or a negative errno.
 */
extern int drm_sync_file_install(struct dma_fence *fence, bool cloexec,
    int *fdp);

/*
 * The fence of sync file `fd` of the calling process with a new
 * reference, or NULL (not a sync file). Only inside an ioctl handler.
 */
extern struct dma_fence *sync_file_get_fence(int fd);

/*
 * sync_file_get_fence() with the reason of a failure (an illumos
 * addition): 0 and the fence in *fence, or -EBADF (no such descriptor),
 * -EINVAL (not a sync file), -ENOTSUP (a sync file of a device whose
 * fences are signalled at another interrupt priority).
 */
extern int drm_illumos_sync_file_get_fence(int fd, struct dma_fence **fence);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_SYNCOBJ_H */
