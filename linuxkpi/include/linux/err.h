/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's error pointers (include/linux/err.h, GPL-2.0). ERR_PTR(),
 * PTR_ERR(), IS_ERR() and IS_ERR_OR_NULL() are the DRM facade's; this adds
 * ERR_CAST() and PTR_ERR_OR_ZERO().
 */
#ifndef _LINUX_ERR_H
#define	_LINUX_ERR_H

#include <linux/types.h>
#include <linux/errno.h>

static inline void *
ERR_CAST(const void *ptr)
{
	return ((void *)(uintptr_t)ptr);
}

static inline int
PTR_ERR_OR_ZERO(const void *ptr)
{
	return (IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0);
}

#define	INIT_ERR_PTR(e)	((void *)(long)(e))

#endif
