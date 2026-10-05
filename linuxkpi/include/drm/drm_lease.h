/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The interface of Linux's DRM leases (include/drm/drm_lease.h,
 * GPL-2.0-or-later; declarations reimplemented, decision D7). Leases are
 * the DRM core's (mode setting); the KMS headers only declare them.
 */
#ifndef _LKPI_DRM_LEASE_H
#define	_LKPI_DRM_LEASE_H
#include <linux/types.h>
struct drm_file;
struct drm_device;
struct drm_master;
extern struct drm_master *drm_lease_owner(struct drm_master *master);
extern void drm_lease_destroy(struct drm_master *lessee);
extern bool drm_lease_held(struct drm_file *file_priv, int id);
extern bool _drm_lease_held(struct drm_file *file_priv, int id);
extern void drm_lease_revoke(struct drm_master *master);
extern uint32_t drm_lease_filter_crtcs(struct drm_file *file_priv,
    uint32_t crtcs);
#endif
