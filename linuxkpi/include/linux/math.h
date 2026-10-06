/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's integer arithmetic helpers (include/linux/math.h, GPL-2.0;
 * reimplemented).
 */
#ifndef _LINUX_MATH_H
#define	_LINUX_MATH_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/const.h>

#define	__round_mask(x, y)	((__typeof__(x))((y) - 1))
#define	round_up(x, y)		((((x) - 1) | __round_mask(x, y)) + 1)
#define	round_down(x, y)	((x) & ~__round_mask(x, y))
#undef	roundup
#define	roundup(x, y) ({ typeof (y) __y = (y); \
	(((x) + (__y - 1)) / __y) * __y; })
#define	rounddown(x, y) ({ typeof (x) __x = (x); __x - (__x % (y)); })
#define	DIV_ROUND_UP(n, d)	__KERNEL_DIV_ROUND_UP(n, d)
#define	DIV_ROUND_DOWN_ULL(ll, d) ({ unsigned long long _tmp = (ll); \
	(void)do_div(_tmp, d); _tmp; })
#define	DIV_ROUND_UP_ULL(ll, d)	DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))
#define	DIV_ROUND_UP_SECTOR_T(ll, d)	DIV_ROUND_UP_ULL(ll, d)
#define	mult_frac(x, n, d) ({ \
	typeof (x) __x = (x); typeof (n) __n = (n); typeof (d) __d = (d); \
	typeof (x) __q = __x / __d; typeof (x) __r = __x % __d; \
	(__q * __n) + ((__r * __n) / __d); })
#define	abs(x) ({ __auto_type __a = (x); __a < 0 ? -__a : __a; })
#define	abs_diff(a, b) ({ __auto_type __a = (a); __auto_type __b = (b); \
	__a > __b ? (__a - __b) : (__b - __a); })
#define	sector_div(a, b)	do_div(a, b)

static inline u32
reciprocal_scale(u32 val, u32 ep_ro)
{
	return ((u32)(((u64)val * ep_ro) >> 32));
}

/* Linux's int_pow(): base to the power exp (exponentiation by squaring). */
static inline u64
int_pow(u64 base, unsigned int exp)
{
	u64 result = 1;

	while (exp != 0) {
		if (exp & 1)
			result *= base;
		exp >>= 1;
		base *= base;
	}
	return (result);
}

static inline unsigned long
int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (x <= 1)
		return (x);
	m = 1UL << ((63 - __builtin_clzl(x)) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return (y);
}

#include <linux/math64.h>

#endif
