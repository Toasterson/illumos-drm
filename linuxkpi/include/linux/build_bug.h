/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's compile-time assertions (include/linux/build_bug.h, GPL-2.0;
 * reimplemented with _Static_assert and the negative bit-field idiom).
 */
#ifndef _LINUX_BUILD_BUG_H
#define	_LINUX_BUILD_BUG_H

#define	BUILD_BUG_ON_ZERO(e)	((int)(sizeof (struct { int:(-!!(e)); })))
#define	BUILD_BUG_ON_INVALID(e)	((void)(sizeof ((long)(e))))
#define	BUILD_BUG_ON_MSG(cond, msg)	do { \
	extern void __lkpi_build_bug(void) \
	    __attribute__((__error__(msg))); \
	if (cond) \
		__lkpi_build_bug(); \
	} while (0)
#define	BUILD_BUG_ON(cond)	BUILD_BUG_ON_MSG(cond, "BUILD_BUG_ON failed: " #cond)
#define	BUILD_BUG_ON_NOT_POWER_OF_2(n)	BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))
#define	BUILD_BUG()		BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")
#define	__lkpi_sa1(expr)	_Static_assert(expr, #expr)
#define	__lkpi_sa2(expr, msg)	_Static_assert(expr, msg)
#define	__lkpi_sasel(_1, _2, n, ...)	n
#define	static_assert(...)	\
	__lkpi_sasel(__VA_ARGS__, __lkpi_sa2, __lkpi_sa1, x)(__VA_ARGS__)
#define	__static_assert(e, msg, ...)	_Static_assert(e, msg)
#define	ASSERT_STRUCT_OFFSET(type, field, expected_offset) \
	static_assert(offsetof(type, field) == (expected_offset))

#endif
