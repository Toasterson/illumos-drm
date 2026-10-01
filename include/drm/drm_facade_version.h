/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The version of the illumos-drm facade and the sizes of its structures.
 * MIT-licensed.
 *
 * Version policy. The major version changes whenever a structure's size
 * or a field's offset or meaning changes, or a function is removed or
 * changes its meaning; the minor version changes when functions or
 * callbacks are added at the end of a structure's reserved space without
 * changing any size. A driver loads only on a core of the same major
 * version and the same or a later minor version, and only if every
 * structure size it was compiled with matches the core's
 * (drm_facade_check() at _init(9E); drm_dev_alloc() checks the version
 * again). Version 0 is the first, unstable version: expect a new major
 * version with every facade milestone (public-layout base objects R5b,
 * the compatibility layer R5c, mode setting R5i).
 */

#ifndef _DRM_FACADE_VERSION_H
#define	_DRM_FACADE_VERSION_H

#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	DRM_FACADE_MAJOR	0
#define	DRM_FACADE_MINOR	1

/*
 * The structures whose sizes are part of the version, in the order of
 * drm_facade_version.sizes[]. Append only.
 */
#define	DRM_FACADE_STRUCTS(X) \
	X(drm_device) \
	X(drm_minor) \
	X(drm_file) \
	X(drm_driver) \
	X(drm_illumos_mode_config) \
	X(drm_ioctl_desc) \
	X(drm_gem_object) \
	X(drm_gem_object_funcs) \
	X(drm_vma_offset_node) \
	X(dma_fence) \
	X(dma_fence_ops) \
	X(dma_fence_cb) \
	X(dma_resv) \
	X(dma_buf) \
	X(dma_buf_attachment) \
	X(drm_rkms_funcs) \
	X(drm_rkms_layout) \
	X(drm_rkms_plane_desc) \
	X(drm_rkms_crtc_desc) \
	X(drm_rkms_encoder_desc) \
	X(drm_rkms_connector_desc) \
	X(drm_rkms_mode_limits) \
	X(drm_rkms_mode) \
	X(drm_rkms_crtc_state) \
	X(drm_rkms_plane_state) \
	X(drm_rkms_connector_state) \
	X(drm_rkms_fb_info) \
	X(drm_rkms_rect)

enum drm_facade_struct {
	DRM_FACADE_SZ_drm_device = 0,
	DRM_FACADE_SZ_drm_minor,
	DRM_FACADE_SZ_drm_file,
	DRM_FACADE_SZ_drm_driver,
	DRM_FACADE_SZ_drm_illumos_mode_config,
	DRM_FACADE_SZ_drm_ioctl_desc,
	DRM_FACADE_SZ_drm_gem_object,
	DRM_FACADE_SZ_drm_gem_object_funcs,
	DRM_FACADE_SZ_drm_vma_offset_node,
	DRM_FACADE_SZ_dma_fence,
	DRM_FACADE_SZ_dma_fence_ops,
	DRM_FACADE_SZ_dma_fence_cb,
	DRM_FACADE_SZ_dma_resv,
	DRM_FACADE_SZ_dma_buf,
	DRM_FACADE_SZ_dma_buf_attachment,
	DRM_FACADE_SZ_drm_rkms_funcs,
	DRM_FACADE_SZ_drm_rkms_layout,
	DRM_FACADE_SZ_drm_rkms_plane_desc,
	DRM_FACADE_SZ_drm_rkms_crtc_desc,
	DRM_FACADE_SZ_drm_rkms_encoder_desc,
	DRM_FACADE_SZ_drm_rkms_connector_desc,
	DRM_FACADE_SZ_drm_rkms_mode_limits,
	DRM_FACADE_SZ_drm_rkms_mode,
	DRM_FACADE_SZ_drm_rkms_crtc_state,
	DRM_FACADE_SZ_drm_rkms_plane_state,
	DRM_FACADE_SZ_drm_rkms_connector_state,
	DRM_FACADE_SZ_drm_rkms_fb_info,
	DRM_FACADE_SZ_drm_rkms_rect,
	DRM_FACADE_NSIZES
};

#define	DRM_FACADE_MAX_SIZES	64

struct drm_facade_version {
	uint32_t major;
	uint32_t minor;
	uint32_t nsizes;		/* DRM_FACADE_NSIZES of the core */
	uint32_t reserved;
	uint64_t sizes[DRM_FACADE_MAX_SIZES];
};

/*
 * Fills `out` with the core's version and structure sizes. Returns 0, or
 * -EINVAL for NULL.
 */
extern int drm_facade_version(struct drm_facade_version *out);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_FACADE_VERSION_H */
