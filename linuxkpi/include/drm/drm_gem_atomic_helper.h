/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_gem_atomic_helper.h> (GPL-2.0; independent
 * declaration), reduced to the plane helper the carried atomic helpers and
 * i915 use:
 *
 * drm_gem_plane_helper_prepare_fb(plane, state)
 *	if state has a framebuffer and no fence yet, takes the implicit
 *	(write) fence of the reservation object of the framebuffer's first
 *	GEM object and sets it as the plane's fence
 *	(drm_atomic_set_fence_for_plane()); 0 or a negative errno.
 *	misc/drm_kms (drm_kms_stubs.c) implements it.
 */
#ifndef __DRM_GEM_ATOMIC_HELPER_H__
#define	__DRM_GEM_ATOMIC_HELPER_H__

struct drm_plane;
struct drm_plane_state;
struct drm_simple_display_pipe;

int drm_gem_plane_helper_prepare_fb(struct drm_plane *plane,
    struct drm_plane_state *state);

#endif
