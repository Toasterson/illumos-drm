/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's 64-bit division helpers (include/linux/math64.h,
 * asm-generic/div64.h; GPL-2.0; reimplemented for a 64-bit target, where
 * they are plain divisions).
 */
#ifndef _LINUX_MATH64_H
#define	_LINUX_MATH64_H

#include <linux/types.h>

#define	do_div(n, base) ({ \
	u32 __base = (base); u32 __rem; \
	__rem = (u32)((u64)(n) % __base); (n) = (u64)(n) / __base; __rem; })

static inline u64
div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	*remainder = (u32)(dividend % divisor);
	return (dividend / divisor);
}

static inline s64
div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{
	*remainder = (s32)(dividend % divisor);
	return (dividend / divisor);
}

static inline u64
div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return (dividend / divisor);
}

static inline u64
div_u64(u64 dividend, u32 divisor)
{
	return (dividend / divisor);
}

static inline s64
div_s64(s64 dividend, s32 divisor)
{
	return (dividend / divisor);
}

static inline u64
div64_u64(u64 dividend, u64 divisor)
{
	return (dividend / divisor);
}

static inline s64
div64_s64(s64 dividend, s64 divisor)
{
	return (dividend / divisor);
}

#define	div64_ul(x, y)	div64_u64((x), (y))
#define	div64_long(x, y)	div64_s64((x), (y))

static inline u64
mul_u32_u32(u32 a, u32 b)
{
	return ((u64)a * b);
}

static inline u64
mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift)
{
	return ((u64)(((unsigned __int128)a * mul) >> shift));
}

static inline u64
mul_u64_u64_shr(u64 a, u64 mul, unsigned int shift)
{
	return ((u64)(((unsigned __int128)a * mul) >> shift));
}

static inline u64
mul_u64_u32_div(u64 a, u32 mul, u32 divisor)
{
	return ((u64)(((unsigned __int128)a * mul) / divisor));
}

static inline u64
mul_u64_u64_div_u64(u64 a, u64 mul, u64 div)
{
	return ((u64)(((unsigned __int128)a * mul) / div));
}

#ifndef	DIV_ROUND_CLOSEST_ULL
#define	DIV_ROUND_CLOSEST_ULL(x, divisor) ({ \
	typeof (divisor) __d = divisor; \
	unsigned long long _tmp = (x) + (__d) / 2; \
	(void)do_div(_tmp, __d); _tmp; })
#endif
#ifndef	DIV_ROUND_CLOSEST
#define	DIV_ROUND_CLOSEST(x, divisor) ({ \
	typeof (x) __x = (x); typeof (divisor) __d = (divisor); \
	(((typeof (x))-1) > 0 || ((typeof (divisor))-1) > 0 || \
	(((__x) > 0) == ((__d) > 0))) ? \
	(((__x) + ((__d) / 2)) / (__d)) : \
	(((__x) - ((__d) / 2)) / (__d)); })
#endif
#define	DIV64_U64_ROUND_UP(ll, d) ({ u64 _d = (d); div64_u64((ll) + _d - 1, _d); })
#define	DIV64_U64_ROUND_CLOSEST(dividend, divisor) ({ u64 _d = (divisor); \
	div64_u64((dividend) + _d / 2, _d); })
#define	DIV_U64_ROUND_UP(ll, d) ({ u32 _d = (d); div_u64((ll) + _d - 1, _d); })
#define	DIV_U64_ROUND_CLOSEST(dividend, divisor) ({ u32 _d = (divisor); \
	div_u64((dividend) + _d / 2, _d); })
#define	DIV_S64_ROUND_CLOSEST(dividend, divisor) ({ \
	s64 __x = (dividend); s32 __d = (divisor); \
	((__x > 0) == (__d > 0)) ? div_s64(__x + (__d / 2), __d) : \
	div_s64(__x - (__d / 2), __d); })

static inline u64
roundup_u64(u64 x, u32 y)
{
	return (DIV_U64_ROUND_UP(x, y) * y);
}

#endif
