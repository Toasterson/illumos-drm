/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_drv.h>: the facade's driver table and device functions
 * (include_next) with the Linux allocation and registration wrappers of
 * the layer (src/lkpi_drm.c):
 *
 * devm_drm_dev_alloc(parent, driver, type, member) allocates the driver's
 *	structure through the facade (__drm_dev_alloc() on the parent's node),
 *	stores the layer's struct device in drm_device.dev and dma_dev, and
 *	drops the device's reference when the parent's managed resources are
 *	released (detach). The core gets a copy of the driver table whose
 *	ioctl entries go through a trampoline that turns Linux-only error
 *	numbers (-ERESTARTSYS, -ENOTSUPP, ...) into illumos ones.
 * drm_dev_register() registers through the facade and then points the
 *	minors' kdev at the layer's device.
 */
#ifndef _LKPI_DRM_DRV_H
#define	_LKPI_DRM_DRV_H

#include <linux/device.h>
#include <drm/drm_device.h>
#include_next <drm/drm_drv.h>

extern void *lkpi_devm_drm_dev_alloc(struct device *parent,
    const struct drm_driver *driver, size_t size, size_t offset);
extern int lkpi_drm_dev_register(struct drm_device *dev, unsigned long flags);
extern void lkpi_drm_dev_unregister(struct drm_device *dev);

#define	devm_drm_dev_alloc(parent, driver, type, member) \
	((type *)lkpi_devm_drm_dev_alloc((parent), (driver), sizeof (type), \
	    offsetof(type, member)))
#define	drm_dev_register(dev, flags)	lkpi_drm_dev_register((dev), (flags))
#define	drm_dev_unregister(dev)		lkpi_drm_dev_unregister(dev)

static inline bool
drm_core_check_feature(const struct drm_device *dev, u32 feature)
{
	return ((dev->driver_features & feature) != 0);
}

#define	drm_drv_uses_atomic_modeset(dev) \
	drm_core_check_feature((dev), DRIVER_ATOMIC)
#define	drm_dev_has_vblank(dev)		((dev)->num_crtcs != 0)
#define	drm_firmware_drivers_only()	false
#define	DRIVER_COMPUTE_ACCEL		(1 << 7)
#define	DRIVER_GEM_GPUVA		(1 << 8)
#define	DRIVER_CURSOR_HOTSPOT		(1 << 9)

#endif
