/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's DRM master and authentication interface. Masters belong to the
 * DRM core; drm_is_current_master() asks it (src/lkpi_drm.c).
 */
#ifndef _LKPI_DRM_AUTH_H
#define	_LKPI_DRM_AUTH_H
#include <drm/drm_file.h>
extern bool drm_is_current_master(struct drm_file *fpriv);
#endif
