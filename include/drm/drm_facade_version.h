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
 * again). The sizes cover every facade structure and the private tail
 * (__core) of every structure that has one, so a change of the core's
 * private state is caught even when it leaves the structure's size alone.
 *
 * History: 0.1 (R5a) the first, unstable version; 1.0 (R5b) the
 * public-layout base objects: Linux v7.2's fence flag numbering (the
 * signalled bit moved from 2 to 3), drm_device.managed and open_count,
 * fences and GEM reference counts that are the objects' state,
 * drm_printer, and the tails in the handshake; 1.1 (R5c) the
 * DRM_FACADE_DEVICE_T/DRM_FACADE_FILE_T hooks for the LinuxKPI layer (the
 * core no longer reads drm_device.dev after drm_dev_alloc(); layouts
 * unchanged) and the reservation functions the layer builds on
 * (dma_resv_init/fini, dma_resv_get_fences, transactions in
 * dma_resv_lock). Expect a new major version with mode setting (R5i).
 */

#ifndef _DRM_FACADE_VERSION_H
#define	_DRM_FACADE_VERSION_H

#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	DRM_FACADE_MAJOR	1
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
	X(drm_rkms_rect) \
	X(drm_printer)

/*
 * The structures with a private tail (__core), whose sizes follow the
 * structures' in drm_facade_version.sizes[]. Append only.
 */
#define	DRM_FACADE_TAILS(X) \
	X(drm_device) \
	X(drm_file) \
	X(drm_gem_object) \
	X(dma_fence) \
	X(dma_resv) \
	X(dma_buf)

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
	DRM_FACADE_SZ_drm_printer,
	DRM_FACADE_TAIL_drm_device,
	DRM_FACADE_TAIL_drm_file,
	DRM_FACADE_TAIL_drm_gem_object,
	DRM_FACADE_TAIL_dma_fence,
	DRM_FACADE_TAIL_dma_resv,
	DRM_FACADE_TAIL_dma_buf,
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
