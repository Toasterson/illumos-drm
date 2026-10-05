/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_device.h>: the facade's struct drm_device (include_next;
 * drm_device.dev is the layer's struct device, facade 1.1) and the
 * device-lifetime helpers (src/lkpi_drm.c): drm_dev_enter()/exit() guard
 * against a device being unplugged with the layer's SRCU, drm_dev_unplug()
 * marks it unplugged and waits for the readers.
 *
 * Facade 2.0: when Linux's KMS headers are on the include path (the builds
 * of misc/drm_kms and i915n, which put the pinned tree's MIT headers
 * there), this header defines DRM_FACADE_DEVICE_KMS, the hook through
 * which the facade's struct drm_device gets Linux's mode-setting fields
 * under their Linux names: vblank, vbl_lock, max_vblank_count,
 * vblank_event_list and mode_config (Linux's struct drm_mode_config,
 * complete before the facade's struct drm_device is defined). They
 * overlay the device's KMS area __kms, whose size is fixed by the facade;
 * the checks after the facade header refuse a layout that does not fit.
 * Builds without those headers (the layer itself) see only __kms; the
 * structure's layout is the same either way.
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

#if __has_include(<drm/drm_mode_config.h>)
#include <linux/list.h>
#include <drm/drm_mode_config.h>

struct drm_vblank_crtc;

#ifdef	DRM_FACADE_DEVICE_KMS
#error "DRM_FACADE_DEVICE_KMS is defined by <drm/drm_device.h> of the layer"
#endif
#define	DRM_FACADE_DEVICE_KMS \
	struct drm_vblank_crtc *vblank; \
	spinlock_t vbl_lock; \
	u32 max_vblank_count; \
	struct list_head vblank_event_list; \
	struct drm_mode_config mode_config;
#define	LKPI_DRM_KMS	1
#endif

#include_next <drm/drm_device.h>

#ifdef	LKPI_DRM_KMS
/* Linux's mode-setting fields must fit the facade's KMS area. */
struct lkpi_drm_device_kms {
	DRM_FACADE_DEVICE_KMS
};
_Static_assert(sizeof (struct lkpi_drm_device_kms) <=
    sizeof (((struct drm_device *)0)->__kms),
    "Linux's KMS fields of struct drm_device do not fit the facade's "
    "KMS area (DRM_FACADE_KMS_DEVICE_WORDS)");
_Static_assert(_Alignof (struct lkpi_drm_device_kms) <= _Alignof (uint64_t),
    "Linux's KMS fields of struct drm_device need more alignment than "
    "the facade's KMS area has");
#endif

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
