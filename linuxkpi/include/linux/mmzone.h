/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's memory zones: illumos has none; MAX_PAGE_ORDER is Linux's. */
#ifndef _LINUX_MMZONE_H
#define	_LINUX_MMZONE_H
#define	MAX_PAGE_ORDER		10
#define	MAX_ORDER		MAX_PAGE_ORDER
#define	NR_PAGE_ORDERS		(MAX_PAGE_ORDER + 1)
#define	PAGE_ALLOC_COSTLY_ORDER	3
#endif
