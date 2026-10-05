/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_file.h>: the facade's struct drm_file (include_next)
 * and the Linux helpers on it. The handle table is the core's
 * (drm_illumos_gem_for_each_handle() replaces walks of object_idr).
 */
#ifndef _LKPI_DRM_FILE_H
#define	_LKPI_DRM_FILE_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <drm/drm_device.h>
#include_next <drm/drm_file.h>

static inline bool
drm_is_render_client(const struct drm_file *file_priv)
{
	return (file_priv->minor != NULL &&
	    file_priv->minor->type == DRM_MINOR_RENDER);
}

static inline bool
drm_is_primary_client(const struct drm_file *file_priv)
{
	return (file_priv->minor != NULL &&
	    file_priv->minor->type == DRM_MINOR_PRIMARY);
}

#define	drm_file_err(f, fmt, ...)	DRM_ERROR(fmt, ##__VA_ARGS__)

#endif
