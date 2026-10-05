/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The facade header (include_next) for code built against the layer, with
 * <drm/drm_lkms.h> first: facade 2.0's drm_facade_check() takes the size of
 * struct drm_illumos_lkms_funcs (DRM_FACADE_STRUCTS), which the facade's
 * drm_facade.h does not include.
 */
#ifndef _LKPI_DRM_FACADE_H
#define	_LKPI_DRM_FACADE_H

#include <drm/drm_lkms.h>
#include_next <drm/drm_facade.h>

#endif
