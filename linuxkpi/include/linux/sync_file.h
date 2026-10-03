/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sync files. The DRM core makes sync-file descriptors
 * (drm_sync_file_install()) and resolves them (sync_file_get_fence(),
 * both in the facade). sync_file_create() wraps a fence for a descriptor
 * that the layer cannot install itself: it returns a layer sync_file
 * whose fd_install() is replaced by drm_sync_file_install() in
 * lkpi_sync_file_install() (src/lkpi_resv.c).
 */
#ifndef _LINUX_SYNC_FILE_H
#define	_LINUX_SYNC_FILE_H

#include <linux/dma-fence.h>
#include <linux/fs.h>
#include <drm/drm_syncobj.h>

struct sync_file {
	struct lx_file *file;
	char user_name[32];
	struct dma_fence *fence;
};

extern struct sync_file *sync_file_create(struct dma_fence *fence);

#endif
