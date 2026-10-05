/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's container_of() with its type check (include/linux/container_of.h,
 * GPL-2.0; reimplemented). Defined before the facade's unchecked variant.
 */
#ifndef _LINUX_CONTAINER_OF_H
#define	_LINUX_CONTAINER_OF_H

#include <linux/build_bug.h>

#undef	container_of
#define	container_of(ptr, type, member) ({ \
	void *__mptr = (void *)(ptr); \
	_Static_assert(__builtin_types_compatible_p(typeof (*(ptr)), \
	    typeof (((type *)0)->member)) || \
	    __builtin_types_compatible_p(typeof (*(ptr)), void), \
	    "pointer type mismatch in container_of()"); \
	((type *)((char *)__mptr - offsetof(type, member))); })
#define	container_of_const(ptr, type, member)	container_of(ptr, type, member)
#define	typeof_member(T, m)	typeof (((T *)0)->m)

#endif
