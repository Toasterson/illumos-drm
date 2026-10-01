/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * illumos additions to the illumos-drm facade: GEM memory and user
 * mappings through the illumos devmap framework, and fences in a
 * device's own fence domain. Linux has no counterpart of these
 * functions. MIT-licensed.
 */

#ifndef _DRM_ILLUMOS_H
#define	_DRM_ILLUMOS_H

#include <drm/drm_facade_types.h>
#include <sys/ddidevmap.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * The memory of a GEM object. An object made with drm_gem_object_init()
 * has backing 0, system memory from ddi_umem_alloc(9F); a driver may add
 * more backings of the same size (a stand-in for an object that moves
 * between memory regions) with drm_illumos_gem_add_backing(). All of them
 * are zeroed, bound for DMA like backing 0, counted in the device's
 * memory and freed with the object. Add backings before the object gets a
 * handle.
 *
 * drm_illumos_gem_add_backing	returns the new backing's index (1 ...) or
 *				-EINVAL (no backing 0, too many: at most 3
 *				more), -ENOMEM.
 * drm_illumos_gem_umem		the backing's ddi_umem cookie, or NULL.
 * drm_illumos_gem_vaddr	its kernel address, or NULL.
 * drm_illumos_gem_sg_count	the number of its DMA cookies, 0 if it is
 *				not bound for DMA.
 * drm_illumos_gem_sg_cookie	DMA cookie `i` of the backing; 0 or -EINVAL.
 */
extern int drm_illumos_gem_add_backing(struct drm_gem_object *obj);
extern ddi_umem_cookie_t drm_illumos_gem_umem(struct drm_gem_object *obj,
    unsigned int backing);
extern void *drm_illumos_gem_vaddr(struct drm_gem_object *obj,
    unsigned int backing);
extern size_t drm_illumos_gem_sg_count(struct drm_gem_object *obj,
    unsigned int backing);
extern int drm_illumos_gem_sg_cookie(struct drm_gem_object *obj,
    unsigned int backing, size_t i, ddi_dma_cookie_t *cookie);

/*
 * Drops a reference to an object whose last release must not run in the
 * caller's context: it is parked and released by a task queue. For
 * timeout(9F) handlers and other contexts that must not block but that
 * drm_gem_object_put() cannot tell from thread context (it detects only
 * interrupt threads). Any context.
 */
extern void drm_illumos_gem_object_put_deferred(struct drm_gem_object *obj);

/*
 * User mappings of an object, for objects with funcs->illumos_movable.
 * A mapping is one piece of one process's mapping (fork and partial
 * munmap(2) make more pieces). drm_illumos_gem_foreach_mapping() calls
 * `fn` for every piece while holding the object's mapping lock; a piece
 * is only valid inside that call or inside the illumos_mapped and
 * illumos_access callbacks.
 *
 * drm_illumos_mapping_cookie	the devmap handle, for devmap_umem_remap.
 * drm_illumos_mapping_user_offset, _object_offset, _len
 *				the piece's fake offset, its offset in the
 *				object, its length in bytes.
 * drm_illumos_mapping_is_settled
 *				false while the devmap framework still sets
 *				the piece up (before devmap_map(9E)).
 * drm_illumos_mapping_unload	devmap_unload(9F) of the piece: the next
 *				access faults into illumos_access. 0 or an
 *				errno.
 * drm_illumos_fault_load	devmap_load(9F) for the faulting range.
 * drm_illumos_fault_default_access
 *				devmap_default_access(9F).
 */
typedef void (*drm_illumos_mapping_fn)(struct drm_illumos_mapping *map,
    void *arg);

extern void drm_illumos_gem_foreach_mapping(struct drm_gem_object *obj,
    drm_illumos_mapping_fn fn, void *arg);
extern devmap_cookie_t drm_illumos_mapping_cookie(
    const struct drm_illumos_mapping *map);
extern uint64_t drm_illumos_mapping_user_offset(
    const struct drm_illumos_mapping *map);
extern uint64_t drm_illumos_mapping_object_offset(
    const struct drm_illumos_mapping *map);
extern size_t drm_illumos_mapping_len(const struct drm_illumos_mapping *map);
extern bool drm_illumos_mapping_is_settled(
    const struct drm_illumos_mapping *map);
extern int drm_illumos_mapping_unload(struct drm_illumos_mapping *map);
extern int drm_illumos_fault_load(struct drm_illumos_fault *fault);
extern int drm_illumos_fault_default_access(struct drm_illumos_fault *fault);

/*
 * Fences in the device's own fence domain, whose locks have the device's
 * fence priority (drm_dev_illumos_set_fence_priority()) and whose fences
 * a refused detach counts: the Rust drivers' fences.
 *
 * drm_illumos_fence_context_alloc	`num` consecutive contexts.
 * drm_illumos_fence_create		a core-allocated, unsignalled fence with
 *					one reference, or ERR_PTR(-ENOMEM).
 *					May be called from interrupt context
 *					(it does not sleep for memory then).
 */
extern uint64_t drm_illumos_fence_context_alloc(struct drm_device *dev,
    unsigned int num);
extern struct dma_fence *drm_illumos_fence_create(struct drm_device *dev,
    uint64_t context, uint64_t seqno);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_ILLUMOS_H */
