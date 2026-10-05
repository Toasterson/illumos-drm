/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's base-2 logarithms (include/linux/log2.h, GPL-2.0;
 * reimplemented). ilog2() of a constant folds to a constant.
 */
#ifndef _LINUX_LOG2_H
#define	_LINUX_LOG2_H

#include <linux/types.h>

static inline __attribute__((__const__)) int
__ilog2_u32(u32 n)
{
	return (31 - __builtin_clz(n));
}

static inline __attribute__((__const__)) int
__ilog2_u64(u64 n)
{
	return (63 - __builtin_clzll(n));
}

#define	ilog2(n) \
	(__builtin_constant_p(n) ? \
	((n) < 2 ? 0 : 63 - __builtin_clzll((u64)(n))) : \
	(sizeof (n) <= 4 ? __ilog2_u32((u32)(n)) : __ilog2_u64((u64)(n))))

static inline __attribute__((__const__)) bool
is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

static inline __attribute__((__const__)) unsigned long
__roundup_pow_of_two(unsigned long n)
{
	return (n <= 1 ? 1UL : 1UL << (64 - __builtin_clzl(n - 1)));
}

static inline __attribute__((__const__)) unsigned long
__rounddown_pow_of_two(unsigned long n)
{
	return (1UL << (63 - __builtin_clzl(n)));
}

#define	roundup_pow_of_two(n) \
	(__builtin_constant_p(n) ? \
	((n) == 1 ? 1UL : 1UL << (ilog2((n) - 1) + 1)) : \
	__roundup_pow_of_two(n))
#define	rounddown_pow_of_two(n) \
	(__builtin_constant_p(n) ? (1UL << ilog2(n)) : __rounddown_pow_of_two(n))
#define	order_base_2(n)	((n) > 1 ? ilog2((n) - 1) + 1 : 0)
#define	const_ilog2(n)	ilog2(n)

static inline int
__bits_per(unsigned long n)
{
	if (n < 2)
		return (1);
	if (is_power_of_2(n))
		return (order_base_2(n) + 1);
	return (order_base_2(n));
}
#define	bits_per(n)	__bits_per(n)

#endif
