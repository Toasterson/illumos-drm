/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's allocation flags (include/linux/gfp_types.h, GPL-2.0). The
 * values of __GFP_ZERO, __GFP_NOWARN and __GFP_DIRECT_RECLAIM are the DRM
 * facade's (<drm/drm_facade_types.h>), which the layer must share; the
 * others are the layer's. The only distinction illumos makes is
 * __GFP_DIRECT_RECLAIM (may sleep: KM_SLEEP) versus not (KM_NOSLEEP);
 * __GFP_ZERO zeroes. The zone, reclaim-policy and accounting flags are
 * accepted and ignored: illumos kernel memory has no zones and KM_SLEEP
 * waits until memory is available, so __GFP_NORETRY/__GFP_RETRY_MAYFAIL
 * callers (which handle failure) simply get memory or wait.
 */
#ifndef _LINUX_GFP_TYPES_H
#define	_LINUX_GFP_TYPES_H

#include <linux/types.h>

#define	__GFP_DMA		0x00001u
#define	__GFP_HIGHMEM		0x00002u
#define	__GFP_DMA32		0x00004u
#define	__GFP_MOVABLE		0x00008u
#define	__GFP_RECLAIMABLE	0x00010u
#define	__GFP_HIGH		0x00020u
#define	__GFP_IO		0x00040u
#define	__GFP_FS		0x00080u
#define	__GFP_KSWAPD_RECLAIM	0x00800u
#define	__GFP_RETRY_MAYFAIL	0x01000u
#define	__GFP_NOFAIL		0x02000u
#define	__GFP_NORETRY		0x04000u
#define	__GFP_MEMALLOC		0x08000u
#define	__GFP_COMP		0x10000u
#define	__GFP_NOMEMALLOC	0x20000u
#define	__GFP_HARDWALL		0x40000u
#define	__GFP_THISNODE		0x80000u
#define	__GFP_ACCOUNT		0x100000u
#define	__GFP_NOLOCKDEP		0x200000u
#define	__GFP_RECLAIM		(__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)
#define	__GFP_WRITE		0x400000u
#define	__GFP_UNMAPPED		0x800000u

#undef	GFP_KERNEL
#undef	GFP_NOWAIT
#undef	GFP_ATOMIC
#define	GFP_ATOMIC		(__GFP_HIGH | __GFP_KSWAPD_RECLAIM)
#define	GFP_NOWAIT		(__GFP_KSWAPD_RECLAIM)
#define	GFP_KERNEL		(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define	GFP_KERNEL_ACCOUNT	(GFP_KERNEL | __GFP_ACCOUNT)
#define	GFP_NOIO		(__GFP_RECLAIM)
#define	GFP_NOFS		(__GFP_RECLAIM | __GFP_IO)
#define	GFP_USER		(__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define	GFP_DMA			__GFP_DMA
#define	GFP_DMA32		__GFP_DMA32
#define	GFP_HIGHUSER		(GFP_USER | __GFP_HIGHMEM)
#define	GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE)
#define	GFP_TRANSHUGE_LIGHT	(GFP_HIGHUSER_MOVABLE | __GFP_COMP | __GFP_NOMEMALLOC | __GFP_NOWARN)
#define	GFP_TRANSHUGE		(GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)
#define	GFP_RECLAIM_MASK	(__GFP_RECLAIM | __GFP_HIGH | __GFP_IO | __GFP_FS | __GFP_NOWARN | \
				__GFP_RETRY_MAYFAIL | __GFP_NOFAIL | __GFP_NORETRY | __GFP_MEMALLOC | \
				__GFP_NOMEMALLOC)
#define	__GFP_BITS_SHIFT	24
#define	__GFP_BITS_MASK		((gfp_t)((1u << __GFP_BITS_SHIFT) - 1))

#endif
