/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The KMS helpers i915's core code calls on its display paths, for the
 * headless build only (DISPLAY=0 in build.sh): without the display the
 * device has no mode configuration, and these do nothing. The display
 * build leaves this file out (display-tus.txt) and links the real
 * helpers of misc/drm_kms instead; a definition here would shadow
 * them.
 */

#include "i915_drv.h"

#include <drm/drm_atomic_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_probe_helper.h>

void
drm_atomic_helper_shutdown(struct drm_device *dev)
{
	(void) dev;
}

void
drm_kms_helper_poll_disable(struct drm_device *dev)
{
	(void) dev;
}

void
drm_kms_helper_poll_enable(struct drm_device *dev)
{
	(void) dev;
}

void
drm_mode_config_reset(struct drm_device *dev)
{
	(void) dev;
}
