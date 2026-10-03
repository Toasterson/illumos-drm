/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's cache-line constants (L1_CACHE_BYTES is in <lkpi/config.h>).
 */
#ifndef _LINUX_CACHE_H
#define	_LINUX_CACHE_H
#include <linux/kernel.h>
#define	cache_line_size()	L1_CACHE_BYTES
#define	____cacheline_internodealigned_in_smp	__aligned(64)
#endif
