/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's min()/max() family (include/linux/minmax.h, GPL-2.0;
 * reimplemented). Each argument is evaluated once; there is no type
 * check beyond the compiler's usual warnings. MIN() and MAX() are
 * illumos' (<sys/sysmacros.h>), which Linux's match.
 */
#ifndef _LINUX_MINMAX_H
#define	_LINUX_MINMAX_H

#include <linux/compiler.h>

#define	__lkpi_cmp(op, x, y)	((x) op (y) ? (x) : (y))
#define	__lkpi_cmp_once(op, x, y) \
	__lkpi_cmp_once_u(op, x, y, __UNIQUE_ID(x_), __UNIQUE_ID(y_))
#define	__lkpi_cmp_once_u(op, x, y, ux, uy) ({ \
	__auto_type ux = (x); __auto_type uy = (y); \
	__lkpi_cmp(op, ux, uy); })

#define	min(x, y)		__lkpi_cmp_once(<, x, y)
#define	max(x, y)		__lkpi_cmp_once(>, x, y)
#define	umin(x, y)		min((u64)(x) + 0ULL, (u64)(y) + 0ULL)
#define	umax(x, y)		max((u64)(x) + 0ULL, (u64)(y) + 0ULL)
#define	min3(x, y, z)		min((typeof ((x) + (y) + (z)))(x), min((y), (z)))
#define	max3(x, y, z)		max((typeof ((x) + (y) + (z)))(x), max((y), (z)))
#define	min_t(type, x, y)	__lkpi_cmp_once(<, (type)(x), (type)(y))
#define	max_t(type, x, y)	__lkpi_cmp_once(>, (type)(x), (type)(y))
#define	min_array(a, n) ({ typeof ((a)[0]) __m = (a)[0]; size_t __i; \
	for (__i = 1; __i < (n); __i++) if ((a)[__i] < __m) __m = (a)[__i]; __m; })
#define	max_array(a, n) ({ typeof ((a)[0]) __m = (a)[0]; size_t __i; \
	for (__i = 1; __i < (n); __i++) if ((a)[__i] > __m) __m = (a)[__i]; __m; })
#define	min_not_zero(x, y) ({ __auto_type __a = (x); __auto_type __b = (y); \
	__a == 0 ? __b : (__b == 0 ? __a : min(__a, __b)); })
#define	clamp(val, lo, hi)	min(max((val), (lo)), (hi))
#define	clamp_t(type, val, lo, hi) \
	min_t(type, max_t(type, (val), (lo)), (hi))
#define	clamp_val(val, lo, hi)	clamp_t(typeof (val), (val), (lo), (hi))
#define	in_range(val, start, len) \
	((u64)(val) - (u64)(start) < (u64)(len))
#define	in_range64(val, start, len) \
	((u64)(val) - (u64)(start) < (u64)(len))
#define	in_range32(val, start, len) \
	((u32)(val) - (u32)(start) < (u32)(len))
#define	swap(a, b) do { \
	typeof (a) __tmp = (a); (a) = (b); (b) = __tmp; \
	} while (0)
#define	MIN_T(type, a, b)	((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define	MAX_T(type, a, b)	((type)(a) > (type)(b) ? (type)(a) : (type)(b))

#endif
