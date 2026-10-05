/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_device.h>: the facade's struct drm_device (include_next;
 * drm_device.dev is the layer's struct device, facade 1.1) and the
 * device-lifetime helpers (src/lkpi_drm.c): drm_dev_enter()/exit() guard
 * against a device being unplugged with the layer's SRCU, drm_dev_unplug()
 * marks it unplugged and waits for the readers.
 */
#ifndef _LKPI_DRM_DEVICE_H
#define	_LKPI_DRM_DEVICE_H

#include <linux/device.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/idr.h>
#include <linux/srcu.h>
#include <linux/ww_mutex.h>
#include_next <drm/drm_device.h>

#define	DRM_WEDGE_RECOVERY_NONE		(1U << 0)
#define	DRM_WEDGE_RECOVERY_REBIND	(1U << 1)
#define	DRM_WEDGE_RECOVERY_BUS_RESET	(1U << 2)
#define	DRM_WEDGE_RECOVERY_VENDOR	(1U << 3)

extern bool drm_dev_enter(struct drm_device *dev, int *idx);
extern void drm_dev_exit(int idx);
extern void drm_dev_unplug(struct drm_device *dev);
extern int drm_dev_wedged_event(struct drm_device *dev, unsigned long method,
    void *info);

static inline bool
drm_dev_is_unplugged(struct drm_device *dev)
{
	return (READ_ONCE(dev->unplugged));
}

#endif
