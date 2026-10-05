/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The host-test replacement of the layer's prologue (linuxkpi/include/
 * lkpi/prologue.h, which this directory shadows on the include path):
 * the host C library and the illumos stand-ins of <lkpi_host.h> instead
 * of the illumos kernel headers, then the same renames and configuration
 * as the real prologue, so the layer's headers and sources compile
 * unchanged.
 */
#ifndef _LKPI_PROLOGUE_H
#define	_LKPI_PROLOGUE_H

#include <lkpi_host.h>

/* Host macros whose names Linux code defines differently. */
#undef	min
#undef	max
#undef	MIN
#undef	MAX
#undef	ARRAY_SIZE
#undef	roundup
#undef	__aligned
#undef	__packed
#undef	__unused
#undef	__used
#undef	__weak
#undef	__noreturn
#undef	__unreachable
#undef	__deprecated
#undef	__printflike
#undef	ALIGN
#undef	SET
#undef	CLR
#undef	BIT
#undef	PAGE_SIZE
#undef	PAGE_SHIFT
#undef	PAGE_MASK

/* The same structure-tag renames as the real prologue. */
#define	mutex	lx_mutex
#define	page	lx_page
#define	pid	lx_pid
#define	file	lx_file
#define	module	lx_module
#define	kstat	lx_kstat
#define	kmem_cache	lkpi_kmem_cache
#define	resume	lx_resume

#include <lkpi/config.h>

struct device;
struct lx_file;
extern dev_info_t *lkpi_dev_dip(const struct device *dev);
#define	DRM_FACADE_DEVICE_T		struct device
#define	DRM_FACADE_DEVICE_DIP(d)	lkpi_dev_dip(d)
#define	DRM_FACADE_FILE_T		struct lx_file
#define	DRM_FACADE_INODE_T		struct inode

#endif	/* _LKPI_PROLOGUE_H */
