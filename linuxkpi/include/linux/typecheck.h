/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's typecheck(). */
#ifndef _LINUX_TYPECHECK_H
#define	_LINUX_TYPECHECK_H
#define	typecheck(type, x) ({ type __dummy; typeof (x) __dummy2; \
	(void)(&__dummy == &__dummy2); 1; })
#define	typecheck_fn(type, function) ({ typeof (type) __tmp = function; \
	(void)__tmp; })
#define	typecheck_pointer(x) ({ typeof (x) __dummy; (void)sizeof (*__dummy); 1; })
#endif
