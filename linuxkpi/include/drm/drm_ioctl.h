/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_ioctl.h>: the facade's driver ioctl table (include_next)
 * and drm_noop()/drm_invalid_op().
 */
#ifndef _LKPI_DRM_IOCTL_H
#define	_LKPI_DRM_IOCTL_H

#include <asm/ioctl.h>
#include <drm/drm_device.h>
#include_next <drm/drm_ioctl.h>

struct drm_file;
extern int drm_noop(struct drm_device *dev, void *data, struct drm_file *file);
extern int drm_invalid_op(struct drm_device *dev, void *data,
    struct drm_file *file);
#define	DRM_UNLOCKED	0

#endif
