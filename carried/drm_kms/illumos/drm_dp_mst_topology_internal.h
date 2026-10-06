/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Replaces Linux's drivers/gpu/drm/display/drm_dp_mst_topology_internal.h
 * (GPL-2.0-only) in the pinned tree (drivers/i915/fetch-and-patch.sh):
 * the declarations of the DP MST sideband message functions that
 * drm_dp_mst_topology.c defines non-static for Linux's self-tests. Only
 * the names and signatures are taken; drm_dp_mst_topology.c (MIT) defines
 * them.
 */

#ifndef _DRM_DP_MST_HELPER_INTERNAL_H_
#define	_DRM_DP_MST_HELPER_INTERNAL_H_

struct drm_dp_sideband_msg_req_body;
struct drm_dp_sideband_msg_tx;
struct drm_printer;

/* Encodes a sideband request body into a message. */
void drm_dp_encode_sideband_req(const struct drm_dp_sideband_msg_req_body *,
    struct drm_dp_sideband_msg_tx *);
/* Decodes a message into a request body: 0 or a negative errno. */
int drm_dp_decode_sideband_req(const struct drm_dp_sideband_msg_tx *,
    struct drm_dp_sideband_msg_req_body *);
/* Prints a request body, indented, for debugging. */
void drm_dp_dump_sideband_msg_req_body(
    const struct drm_dp_sideband_msg_req_body *, int, struct drm_printer *);

#endif
