/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_managed.h>: the facade's drmm_*() (include_next) and
 * drmm_mutex_init() (a mutex destroyed with the device's resources).
 */
#ifndef _LKPI_DRM_MANAGED_H
#define	_LKPI_DRM_MANAGED_H

#include <linux/mutex.h>
#include <drm/drm_device.h>
#include_next <drm/drm_managed.h>

extern void __drmm_mutex_release(struct drm_device *dev, void *res);

static inline int
lkpi_drmm_mutex_init(struct drm_device *dev, struct mutex *lock)
{
	mutex_init(lock);
	return (drmm_add_action_or_reset(dev, __drmm_mutex_release, lock));
}
#define	drmm_mutex_init(dev, lock)	lkpi_drmm_mutex_init((dev), (lock))

#endif
