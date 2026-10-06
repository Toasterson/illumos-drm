/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's fixed-point logarithms (include/linux/int_log.h,
 * LGPL-2.1-or-later; reimplemented in src/lkpi_int_log.c with integer
 * arithmetic only).
 *
 * intlog2(value)
 *	log2(value) in 8.24 fixed point (log2(value) * 2^24): intlog2(8) is
 *	3 << 24. Fractions: intlog2(v * 2^x) - (x << 24).
 * intlog10(value)
 *	log10(value) in 8.24 fixed point: intlog10(1000) is 3 << 24.
 *	Fractions: intlog10(v * 10^x) - (x << 24).
 * Both round to the nearest 2^-24 (Linux's table interpolation is
 * approximate and may be a unit off; here exact powers of 2 and of 10
 * give exact results). value 0 WARNs and returns 0, as on Linux.
 * Neither sleeps; both work in any context.
 */
#ifndef _LINUX_INT_LOG_H
#define	_LINUX_INT_LOG_H

#include <linux/types.h>

extern unsigned int intlog2(u32 value);
extern unsigned int intlog10(u32 value);

#endif
