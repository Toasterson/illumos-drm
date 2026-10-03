/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's random numbers over illumos random_get_pseudo_bytes(9F)
 * (src/lkpi_misc.c).
 */
#ifndef _LINUX_RANDOM_H
#define	_LINUX_RANDOM_H
#include <linux/types.h>
#include <linux/prandom.h>
extern void get_random_bytes(void *buf, size_t len);
extern u32 get_random_u32(void);
extern u64 get_random_u64(void);
#define	get_random_u8()		((u8)get_random_u32())
#define	get_random_u16()	((u16)get_random_u32())
#define	get_random_long()	((unsigned long)get_random_u64())
#define	get_random_int()	get_random_u32()
static inline u32
get_random_u32_below(u32 ceil)
{
	return (ceil == 0 ? 0 : (u32)(((u64)get_random_u32() * ceil) >> 32));
}
#define	get_random_u32_inclusive(lo, hi)	((lo) + get_random_u32_below((hi) - (lo) + 1))
#endif
