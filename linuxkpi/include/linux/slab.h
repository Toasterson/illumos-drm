/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Stub of Linux's include/linux/slab.h for the carried helpers
 * (carried/). drm_mm.c includes it but, built without
 * CONFIG_DRM_DEBUG_MM as here, allocates nothing, so this stub declares
 * no allocator. Linux's slab.h brings the error numbers and the general
 * kernel helpers in through its own includes, and drm_mm.c depends on
 * that (ENOSPC), so this stub includes them.
 *
 * To be reconciled with R5c: R5c's slab.h (kmalloc() and friends over
 * kmem_alloc(9F)) replaces this stub.
 */

#ifndef _LINUXKPI_LINUX_SLAB_H
#define	_LINUXKPI_LINUX_SLAB_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>

#endif	/* _LINUXKPI_LINUX_SLAB_H */
