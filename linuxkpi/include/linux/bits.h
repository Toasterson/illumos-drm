/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's BIT() and GENMASK() (include/linux/bits.h, vdso/bits.h,
 * GPL-2.0; reimplemented as integer constant expressions without the
 * input checks).
 */
#ifndef _LINUX_BITS_H
#define	_LINUX_BITS_H

#include <linux/const.h>

#define	BIT(nr)			(1UL << (nr))
#define	BIT_ULL(nr)		(1ULL << (nr))
#define	BIT_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define	BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define	BIT_ULL_MASK(nr)	(1ULL << ((nr) % BITS_PER_LONG_LONG))
#define	BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define	BIT_U8(nr)		((u8)(1U << (nr)))
#define	BIT_U16(nr)		((u16)(1U << (nr)))
#define	BIT_U32(nr)		((u32)(1U << (nr)))
#define	BIT_U64(nr)		((u64)(1ULL << (nr)))

#define	GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define	GENMASK_ULL(h, l) \
	(((~0ULL) - (1ULL << (l)) + 1) & \
	(~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#define	GENMASK_U8(h, l)	((u8)GENMASK(h, l))
#define	GENMASK_U16(h, l)	((u16)GENMASK(h, l))
#define	GENMASK_U32(h, l)	((u32)GENMASK(h, l))
#define	GENMASK_U64(h, l)	((u64)GENMASK_ULL(h, l))
#define	GENMASK_U128(h, l) \
	((((u128)~(u128)0) - ((u128)1 << (l)) + 1) & \
	((u128)~(u128)0 >> (127 - (h))))
#define	GENMASK_TYPE(t, h, l)	((t)GENMASK_ULL(h, l))

#endif
