/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's util_macros.h (GPL-2.0; reimplemented): find_closest() and
 * friends, PTR_IF(), is_insidevar().
 */
#ifndef _LINUX_UTIL_MACROS_H
#define	_LINUX_UTIL_MACROS_H

#include <linux/math.h>

#define	__find_closest(x, a, as, op) ({ \
	typeof (as) __fc_i, __fc_as = (as) - 1; \
	typeof (x) __fc_x = (x); \
	typeof (*a) const *__fc_a = (a); \
	for (__fc_i = 0; __fc_i < __fc_as; __fc_i++) { \
		if (__fc_x op DIV_ROUND_CLOSEST(__fc_a[__fc_i] + \
		    __fc_a[__fc_i + 1], 2)) \
			break; \
	} \
	(__fc_i); })
#define	find_closest(x, a, as)		__find_closest(x, a, as, <=)
#define	find_closest_descending(x, a, as)	__find_closest(x, a, as, >=)
#define	PTR_IF(cond, ptr)	((cond) ? (ptr) : NULL)
#define	is_insidevar(ptr, var) \
	((uintptr_t)(ptr) >= (uintptr_t)(var) && \
	(uintptr_t)(ptr) < (uintptr_t)(var) + sizeof (var))
#define	u64_to_user_ptr(x)	((void __user *)(uintptr_t)(u64)(x))
#define	UNIQUE_ID(prefix)	__UNIQUE_ID(prefix)

#endif
