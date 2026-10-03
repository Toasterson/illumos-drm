/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's general kernel helpers for the carried helpers (carried/):
 * min()/max() with Linux's type check, min_t()/max_t(), ARRAY_SIZE(),
 * and the headers Linux's kernel.h brings in that they use.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's include/linux/kernel.h,
 * minmax.h and array_size.h (GPL-2.0). This file is a reimplementation,
 * not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_KERNEL_H
#define	_LINUXKPI_LINUX_KERNEL_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/limits.h>
#include <linux/bug.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/math64.h>

#ifndef	ARRAY_SIZE
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#endif

/*
 * min() and max() evaluate each argument once; like Linux's, they refuse
 * (with -Werror) arguments of different types, through the comparison of
 * two pointers to them. min_t() and max_t() convert both to `type`.
 */
#define	__lkpi_minmax(x, y, op) __extension__ ({			\
	__typeof__(x) __lkpi_mm_x = (x);				\
	__typeof__(y) __lkpi_mm_y = (y);				\
	(void) (&__lkpi_mm_x == &__lkpi_mm_y);				\
	__lkpi_mm_x op __lkpi_mm_y ? __lkpi_mm_x : __lkpi_mm_y;		\
})
#undef	min
#undef	max
#define	min(x, y)	__lkpi_minmax((x), (y), <)
#define	max(x, y)	__lkpi_minmax((x), (y), >)

#define	min_t(type, x, y) __extension__ ({				\
	type __lkpi_mt_x = (x);						\
	type __lkpi_mt_y = (y);						\
	__lkpi_mt_x < __lkpi_mt_y ? __lkpi_mt_x : __lkpi_mt_y;		\
})
#define	max_t(type, x, y) __extension__ ({				\
	type __lkpi_mt_x = (x);						\
	type __lkpi_mt_y = (y);						\
	__lkpi_mt_x > __lkpi_mt_y ? __lkpi_mt_x : __lkpi_mt_y;		\
})

#endif	/* _LINUXKPI_LINUX_KERNEL_H */
