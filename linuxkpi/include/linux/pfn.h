/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's PFN helpers. */
#ifndef _LINUX_PFN_H
#define	_LINUX_PFN_H
#include <linux/types.h>
#define	PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)
#define	PFN_UP(x)	(((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define	PFN_DOWN(x)	((x) >> PAGE_SHIFT)
#define	PFN_PHYS(x)	((phys_addr_t)(x) << PAGE_SHIFT)
#define	PHYS_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))
#endif
