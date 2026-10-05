/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Managed resources of a DRM device in the illumos-drm facade.
 *
 * Origin: the functions mirror the names and semantics of Linux v7.2's
 * include/drm/drm_managed.h. That header is GPL-2.0; this file is a
 * reimplementation for illumos from the documented interface, not a copy,
 * and is MIT-licensed (decision D7 of illumos-drm, 2026-10-01).
 *
 * A driver ties actions and memory to the life of its struct drm_device.
 * They are released when the device's last reference goes
 * (drm_dev_put()): after drm_dev_unregister() and the driver's release
 * callback, before the device structure is freed, in the reverse order
 * of their registration, each action called and each allocation freed
 * once, with no lock held, in thread context.
 *
 * Provided: drmm_add_action(), drmm_add_action_or_reset(),
 * drmm_release_action(), drmm_kmalloc(), drmm_kzalloc(),
 * drmm_kmalloc_array(), drmm_kcalloc(), drmm_kstrdup(), drmm_kfree().
 * Not provided: drmm_mutex_init() and drmm_alloc_ordered_workqueue(),
 * which need Linux's struct mutex and workqueues (the compatibility
 * layer); a driver registers mutex_destroy() as an action meanwhile.
 */

#ifndef _DRM_MANAGED_H
#define	_DRM_MANAGED_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef void (*drmres_release_t)(struct drm_device *dev, void *res);

/*
 * Registers `action(dev, data)` for the device's release. Returns 0,
 * -EINVAL (no action) or -ENOMEM. `name` is kept for debuggers. Thread
 * context.
 */
extern int __drmm_add_action(struct drm_device *dev, drmres_release_t action,
    void *data, const char *name);
#define	drmm_add_action(dev, action, data) 	__drmm_add_action(dev, action, data, #action)

/* As __drmm_add_action(), but a failure calls the action at once. */
extern int __drmm_add_action_or_reset(struct drm_device *dev,
    drmres_release_t action, void *data, const char *name);
#define	drmm_add_action_or_reset(dev, action, data) 	__drmm_add_action_or_reset(dev, action, data, #action)

/*
 * Calls the oldest registration of `action` with `data` now and removes
 * it, so that the device's release does not call it again; a warning if
 * there is none. Thread context.
 */
extern void drmm_release_action(struct drm_device *dev,
    drmres_release_t action, void *data);

/*
 * `size` bytes, 8-byte aligned, freed at the device's release or by
 * drmm_kfree(); zeroed with __GFP_ZERO. NULL for 0 bytes or without
 * memory (only without __GFP_DIRECT_RECLAIM). Thread context with
 * GFP_KERNEL, any context up to low-level interrupts otherwise.
 */
extern void *drmm_kmalloc(struct drm_device *dev, size_t size, gfp_t gfp);

static inline void *
drmm_kzalloc(struct drm_device *dev, size_t size, gfp_t gfp)
{
	return (drmm_kmalloc(dev, size, gfp | __GFP_ZERO));
}

static inline void *
drmm_kmalloc_array(struct drm_device *dev, size_t n, size_t size,
    gfp_t gfp)
{
	if (size != 0 && n > ((size_t)-1) / size)
		return (NULL);
	return (drmm_kmalloc(dev, n * size, gfp));
}

static inline void *
drmm_kcalloc(struct drm_device *dev, size_t n, size_t size, gfp_t gfp)
{
	return (drmm_kmalloc_array(dev, n, size, gfp | __GFP_ZERO));
}

/* A managed copy of a string; NULL for NULL or without memory. */
extern char *drmm_kstrdup(struct drm_device *dev, const char *s, gfp_t gfp);

/*
 * Frees a managed allocation early; NULL is allowed, anything else that
 * is no managed allocation of `dev` is left alone with a warning.
 */
extern void drmm_kfree(struct drm_device *dev, void *data);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_MANAGED_H */
