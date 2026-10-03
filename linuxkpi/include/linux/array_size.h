/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's ARRAY_SIZE() with its array check. */
#ifndef _LINUX_ARRAY_SIZE_H
#define	_LINUX_ARRAY_SIZE_H
#include <linux/compiler.h>
#undef	ARRAY_SIZE
#define	ARRAY_SIZE(arr)	(sizeof (arr) / sizeof ((arr)[0]) + __must_be_array(arr))
#define	ARRAY_END(arr)	(&(arr)[ARRAY_SIZE(arr)])
#endif
