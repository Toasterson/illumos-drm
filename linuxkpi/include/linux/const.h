/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's _AC() and friends. */
#ifndef _LINUX_CONST_H
#define	_LINUX_CONST_H
#define	__AC(X, Y)	(X##Y)
#define	_AC(X, Y)	__AC(X, Y)
#define	_AT(T, X)	((T)(X))
#define	_UL(x)		(_AC(x, UL))
#define	_ULL(x)		(_AC(x, ULL))
#define	UL(x)		(_UL(x))
#define	ULL(x)		(_ULL(x))
#define	_BITUL(x)	(_UL(1) << (x))
#define	_BITULL(x)	(_ULL(1) << (x))
#define	__ALIGN_KERNEL(x, a)	__ALIGN_KERNEL_MASK(x, (__typeof__(x))(a) - 1)
#define	__ALIGN_KERNEL_MASK(x, mask)	(((x) + (mask)) & ~(mask))
#define	__KERNEL_DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif
