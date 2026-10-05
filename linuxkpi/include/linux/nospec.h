/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's speculation barrier for array indices (include/linux/nospec.h,
 * GPL-2.0; reimplemented): the index is clamped to 0 when out of bounds
 * through a data dependency (a mask computed without a branch).
 */
#ifndef _LINUX_NOSPEC_H
#define	_LINUX_NOSPEC_H
#include <linux/compiler.h>
static inline unsigned long
array_index_mask_nospec(unsigned long index, unsigned long size)
{
	unsigned long mask;

	__asm__ ("cmp %1,%2; sbb %0,%0;" : "=r" (mask) : "g" (size), "r" (index) : "cc");
	return (mask);
}
#define	array_index_nospec(index, size) ({ \
	typeof (index) _i = (index); \
	typeof (size) _s = (size); \
	unsigned long _mask = array_index_mask_nospec(_i, _s); \
	(typeof (_i))(_i & _mask); })
#endif
