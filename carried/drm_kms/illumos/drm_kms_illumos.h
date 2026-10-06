/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * misc/drm_kms' illumos sources: what they share among themselves, and the
 * hook table drivers use (drm_kms_illumos_funcs, exported).
 */
#ifndef _DRM_KMS_ILLUMOS_H
#define	_DRM_KMS_ILLUMOS_H

#include <drm/drm_lkms.h>

struct drm_device;
struct drm_file;

/*
 * The facade 2.0 hooks of a Linux KMS device: a driver with
 * DRIVER_MODESET sets drm_driver.illumos_lkms = &drm_kms_illumos_funcs
 * (drm_kms_lkms.c).
 */
extern const struct drm_illumos_lkms_funcs drm_kms_illumos_funcs;

/* Unlinks a closing file's pending events (drm_kms_event.c). */
extern void drm_kms_events_release(struct drm_device *dev,
    struct drm_file *file);

#endif
