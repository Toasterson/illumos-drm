/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The OpRegion VBT reader of the illumos build (i915_illumos_vbt.c),
 * which patch 0013's intel_bios_get_vbt() calls after
 * intel_opregion_get_vbt(): a kmalloc() copy of the VBT and its size in
 * *sizep, or NULL.
 */
#ifndef _I915_ILLUMOS_VBT_H
#define	_I915_ILLUMOS_VBT_H

#include <linux/types.h>

struct intel_display;

extern const void *i915_illumos_opregion_vbt(struct intel_display *display,
    size_t *sizep);

#endif
