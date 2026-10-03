/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's list poison values (non-canonical addresses on x86-64). */
#ifndef _LINUX_POISON_H
#define	_LINUX_POISON_H
#define	POISON_POINTER_DELTA	0xdead000000000000UL
#define	LIST_POISON1	((void *)(0x100 + POISON_POINTER_DELTA))
#define	LIST_POISON2	((void *)(0x122 + POISON_POINTER_DELTA))
#define	TIMER_ENTRY_STATIC	((void *)(0x300 + POISON_POINTER_DELTA))
#define	POISON_INUSE	0x5a
#define	POISON_FREE	0x6b
#define	POISON_END	0xa5
#define	PAGE_POISON	0xaa
#endif
