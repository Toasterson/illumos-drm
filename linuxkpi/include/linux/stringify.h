/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's __stringify(). */
#ifndef _LINUX_STRINGIFY_H
#define	_LINUX_STRINGIFY_H
#define	__stringify_1(x...)	#x
#define	__stringify(x...)	__stringify_1(x)
#define	FILE_LINE		__FILE__ ":" __stringify(__LINE__)
#endif
