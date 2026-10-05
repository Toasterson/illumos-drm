/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_syncobj.h>: the facade's sync objects (include_next)
 * (drm_syncobj_create() and drm_syncobj_fence_get() are the facade's
 * since 1.1; sync objects are the core's and opaque to C).
 */
#ifndef _LKPI_DRM_SYNCOBJ_H
#define	_LKPI_DRM_SYNCOBJ_H

#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <drm/drm_file.h>
#include_next <drm/drm_syncobj.h>

#define	DRM_SYNCOBJ_CREATE_SIGNALED	(1 << 0)


#endif
