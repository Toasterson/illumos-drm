/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's attribute shorthands (include/linux/compiler_attributes.h,
 * GPL-2.0), written for gcc.
 */
#ifndef _LINUX_COMPILER_ATTRIBUTES_H
#define	_LINUX_COMPILER_ATTRIBUTES_H

#define	__aligned(x)			__attribute__((__aligned__(x)))
#define	__aligned_largest		__attribute__((__aligned__))
#define	__alias(s)			__attribute__((__alias__(#s)))
#define	__always_inline			inline __attribute__((__always_inline__))
#define	__always_unused			__attribute__((__unused__))
#define	__maybe_unused			__attribute__((__unused__))
#define	__cold				__attribute__((__cold__))
#define	__attribute_const__		__attribute__((__const__))
#define	__copy(sym)			__attribute__((__copy__(sym)))
#define	__counted_by(member)
#define	__counted_by_le(member)
#define	__counted_by_be(member)
#define	__deprecated
#define	__designated_init
#define	__fallthrough			__attribute__((__fallthrough__))
#define	fallthrough			__attribute__((__fallthrough__))
#define	__flatten			__attribute__((__flatten__))
#define	__gnu_inline			__attribute__((__gnu_inline__))
#define	__malloc			__attribute__((__malloc__))
#define	__mode(x)			__attribute__((__mode__(x)))
#define	__must_check			__attribute__((__warn_unused_result__))
#define	__noinline			__attribute__((__noinline__))
#define	noinline			__attribute__((__noinline__))
#define	__nonstring			__attribute__((__nonstring__))
#define	__noreturn			__attribute__((__noreturn__))
#define	__packed			__attribute__((__packed__))
#define	__pure				__attribute__((__pure__))
#define	__printf(a, b)			__attribute__((__format__(printf, a, b)))
#define	__scanf(a, b)			__attribute__((__format__(scanf, a, b)))
#define	__section(s)			__attribute__((__section__(s)))
#define	__used				__attribute__((__used__))
#define	__unused			__attribute__((__unused__))
#define	__weak				__attribute__((__weak__))
#define	__cleanup(func)			__attribute__((__cleanup__(func)))
#define	__no_sanitize_address
#define	__no_sanitize_thread
#define	__no_sanitize_undefined
#define	__noclone			__attribute__((__noclone__))
#define	__assume_aligned(a, ...)	\
	__attribute__((__assume_aligned__(a, ## __VA_ARGS__)))
#define	__nocf_check
#define	__no_stack_protector
#define	__overloadable
#define	__access(...)
#define	__no_kmsan_checks
#define	__no_profile
#define	__returns_nonnull		__attribute__((__returns_nonnull__))
#define	__alloc_size(x, ...)
#define	__realloc_size(x, ...)
#define	__nonnull(...)

#endif
