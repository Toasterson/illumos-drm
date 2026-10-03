/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's compiler helpers (include/linux/compiler.h, GPL-2.0;
 * reimplemented). READ_ONCE()/WRITE_ONCE() are volatile accesses of the
 * object's own size, as on Linux; the memory barriers are in <asm/barrier.h>.
 */
#ifndef _LINUX_COMPILER_H
#define	_LINUX_COMPILER_H

#include <linux/compiler_types.h>

#define	likely(x)		__builtin_expect(!!(x), 1)
#define	unlikely(x)		__builtin_expect(!!(x), 0)
#define	likely_notrace(x)	likely(x)
#define	unlikely_notrace(x)	unlikely(x)
#define	barrier()		__asm__ __volatile__("" : : : "memory")
#define	barrier_data(p)		__asm__ __volatile__("" : : "r"(p) : "memory")
#define	unreachable()		__builtin_unreachable()
#define	OPTIMIZER_HIDE_VAR(var)	__asm__ ("" : "=r" (var) : "0" (var))
#define	RELOC_HIDE(p, off)	({ unsigned long __p = (unsigned long)(p); \
	(typeof (p))(__p + (off)); })

#define	__READ_ONCE(x)		(*(const volatile typeof (x) *)&(x))
#define	READ_ONCE(x)		__READ_ONCE(x)
#define	__WRITE_ONCE(x, val)	do { \
	*(volatile typeof (x) *)&(x) = (val); \
	} while (0)
#define	WRITE_ONCE(x, val)	__WRITE_ONCE(x, val)
#define	data_race(expr)		(expr)
#define	ASSERT_EXCLUSIVE_WRITER(v)	((void)0)
#define	ASSERT_EXCLUSIVE_ACCESS(v)	((void)0)
#define	kcsan_check_access(p, s, t)	((void)0)

#define	__must_be_array(a)	BUILD_BUG_ON_ZERO(__same_type((a), &(a)[0]))
#define	__must_be_byte_array(a)	0
#define	__must_be_cstr(p)	0
#define	__is_constexpr(x)	__builtin_constant_p(x)
#define	is_signed_type(type)	(((type)(-1)) < (__force type)1)
#define	is_unsigned_type(type)	(!is_signed_type(type))
#define	statically_true(x)	(__builtin_constant_p(x) && (x))
#define	const_true(x)		statically_true(x)
#define	__UNIQUE_ID(prefix)	__lkpi_cat(__lkpi_cat(__UNIQUE_ID_, prefix), __COUNTER__)
#define	__lkpi_cat2(a, b)	a##b
#define	__lkpi_cat(a, b)	__lkpi_cat2(a, b)
#define	___PASTE(a, b)		a##b
#define	__PASTE(a, b)		___PASTE(a, b)
#define	__ADDRESSABLE(sym)
#define	prevent_tail_call_optimization()	barrier()
#define	__scalar_type_to_expr_cases(type) \
	unsigned type: (unsigned type)0, signed type: (signed type)0

#include <linux/build_bug.h>
#include <asm/rwonce.h>

#endif
