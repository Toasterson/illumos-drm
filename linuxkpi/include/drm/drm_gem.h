/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_gem.h>: the facade's GEM objects (include_next) with
 * the Linux helpers built on them. A GEM object's reference count and
 * release are the core's: drm_gem_object_put() is the facade's function,
 * and drm_gem_object_free() (Linux's kref release) is only called by
 * Linux's inline put, which the facade replaces; the layer provides it
 * as a WARN. Object lookups under RCU are not supported (the core frees
 * objects without a grace period, see the facade README).
 */
#ifndef _LKPI_DRM_GEM_H
#define	_LKPI_DRM_GEM_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/dma-resv.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <drm/drm_vma_manager.h>
#include_next <drm/drm_gem.h>

static inline unsigned long
drm_vma_node_size(struct drm_vma_offset_node *node)
{
	return ((unsigned long)node->vm_node.size);
}

static inline bool
drm_vma_node_has_offset(struct drm_vma_offset_node *node)
{
	return (node->vm_node.size != 0);
}

static inline void
drm_vma_node_reset(struct drm_vma_offset_node *node)
{
	node->vm_node.start = 0;
	node->vm_node.size = 0;
	node->driver_private = NULL;
}

/* User mappings of an object are the core's; nothing to zap here. */
#define	drm_vma_node_unmap(node, mapping)	((void)(node), (void)(mapping))
#define	drm_vma_node_verify_access(node, tag)	(drm_vma_node_is_allowed(node, tag) ? 0 : -EACCES)

extern void drm_gem_object_free(struct kref *kref);
extern void drm_gem_free_mmap_offset(struct drm_gem_object *obj);
extern void drm_gem_lru_init_lkpi(void);

static inline void
__drm_gem_object_put(struct drm_gem_object *obj)
{
	drm_gem_object_put(obj);
}

#define	drm_gem_object_put_locked(obj)	drm_gem_object_put(obj)

/*
 * Linux 7's per-device huge-page shmem mount: illumos shmem files are
 * umem allocations without transparent huge pages, so there is none.
 */
struct vfsmount;
#define	drm_gem_get_huge_mnt(dev)		((void)(dev), (struct vfsmount *)NULL)
#define	drm_gem_huge_mnt_create(dev, value)	((void)(dev), 0)
#define	DRM_GEM_FOPS			{ }

static inline bool
drm_gem_object_is_shared_for_memory_stats(struct drm_gem_object *obj)
{
	return (obj->handle_count > 1 || obj->dma_buf != NULL);
}

#include <drm/drm_prime.h>

#endif
