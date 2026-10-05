/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's overflow checks (include/linux/overflow.h, GPL-2.0 OR MIT
 * upstream; reimplemented with gcc's __builtin_*_overflow). The size
 * helpers saturate at SIZE_MAX, so an allocation of an overflowing size
 * fails.
 */
#ifndef _LINUX_OVERFLOW_H
#define	_LINUX_OVERFLOW_H

#include <linux/compiler.h>
#include <linux/limits.h>

#define	check_add_overflow(a, b, d)	__builtin_add_overflow(a, b, d)
#define	check_sub_overflow(a, b, d)	__builtin_sub_overflow(a, b, d)
#define	check_mul_overflow(a, b, d)	__builtin_mul_overflow(a, b, d)
#define	check_shl_overflow(a, s, d) ({ \
	typeof (a) _a = (a); typeof (s) _s = (s); typeof (d) _d = (d); \
	u64 _a_full = _a; unsigned int _to_shift = \
	    (_s >= 0 && _s < 8 * sizeof (*d)) ? _s : 0; \
	*_d = (_a_full << _to_shift); \
	(_to_shift != _s || *_d < 0 || _a < 0 || (*_d >> _to_shift) != _a); })
#define	__type_half_max(T) \
	((typeof (T))1 << (8 * sizeof (typeof (T)) - 1 - is_signed_type(typeof (T))))
#define	type_max(T)	((typeof (T))((__type_half_max(T) - 1) + __type_half_max(T)))
#define	type_min(T)	((typeof (T))((typeof (T))-type_max(T) - (typeof (T))1))
#define	__overflows_type_constexpr(x, T) ( \
	is_unsigned_type(typeof (x)) ? (x) > type_max(T) : \
	is_unsigned_type(typeof (T)) ? (x) < 0 || (x) > type_max(T) : \
	(x) < type_min(T) || (x) > type_max(T))
#define	__overflows_type(x, T) ({ typeof (T) v = 0; check_add_overflow((x), v, &v); })
#define	overflows_type(n, T) \
	__builtin_choose_expr(__is_constexpr(n), \
	    __overflows_type_constexpr(n, T), __overflows_type(n, T))
#define	castable_to_type(n, T) \
	__builtin_choose_expr(__is_constexpr(n), \
	    !__overflows_type_constexpr(n, T), __same_type(n, T))
#define	range_overflows(start, size, max) ({ \
	typeof (start) start__ = (start); typeof (size) size__ = (size); \
	typeof (max) max__ = (max); \
	start__ >= max__ || size__ > max__ - start__; })
#define	range_overflows_t(type, start, size, max) \
	range_overflows((type)(start), (type)(size), (type)(max))
#define	range_end_overflows(start, size, max) ({ \
	typeof (start) start__ = (start); typeof (size) size__ = (size); \
	typeof (max) max__ = (max); \
	start__ > max__ || size__ > max__ - start__; })
#define	range_end_overflows_t(type, start, size, max) \
	range_end_overflows((type)(start), (type)(size), (type)(max))

static inline size_t
size_mul(size_t f1, size_t f2)
{
	size_t b;

	if (__builtin_mul_overflow(f1, f2, &b))
		b = SIZE_MAX;
	return (b);
}

static inline size_t
size_add(size_t a, size_t b)
{
	size_t s;

	if (__builtin_add_overflow(a, b, &s))
		s = SIZE_MAX;
	return (s);
}

static inline size_t
size_sub(size_t m, size_t s)
{
	size_t d;

	if (m == SIZE_MAX || s == SIZE_MAX || __builtin_sub_overflow(m, s, &d))
		d = SIZE_MAX;
	return (d);
}

#define	array_size(a, b)	size_mul(a, b)
#define	array3_size(a, b, c)	size_mul(size_mul(a, b), c)
#define	flex_array_size(p, member, count) \
	size_mul(count, sizeof (*(p)->member))
#define	struct_size(p, member, count) \
	size_add(sizeof (*(p)), flex_array_size(p, member, count))
#define	struct_size_t(type, member, count) \
	struct_size((type *)NULL, member, count)
#define	__DEFINE_FLEX(type, name, member, count, trailer...) \
	union { u8 bytes[struct_size_t(type, member, count)]; type obj; } \
	name##_u trailer; type *name = (type *)&name##_u
#define	DEFINE_FLEX(type, name, member, counter, count) \
	__DEFINE_FLEX(type, name, member, count, = { .obj.counter = count })
#define	DEFINE_RAW_FLEX(type, name, member, count) \
	__DEFINE_FLEX(type, name, member, count, = { })
#define	is_non_negative(a)	((a) > 0 || (a) == 0)
#define	is_negative(a)		(!(is_non_negative(a)))
#define	wrapping_add(T, a, b)	((T)((T)(a) + (T)(b)))
#define	wrapping_sub(T, a, b)	((T)((T)(a) - (T)(b)))
#define	wrapping_mul(T, a, b)	((T)((T)(a) * (T)(b)))

#endif
