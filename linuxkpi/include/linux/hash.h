/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's multiplicative hashes (include/linux/hash.h, GPL-2.0;
 * reimplemented with the same golden-ratio constants, which are
 * mathematical constants, so hash values match Linux's).
 */
#ifndef _LINUX_HASH_H
#define	_LINUX_HASH_H

#include <linux/types.h>

#define	GOLDEN_RATIO_32	0x61C88647
#define	GOLDEN_RATIO_64	0x61C8864680B583EBull

static inline u32
__hash_32(u32 val)
{
	return (val * GOLDEN_RATIO_32);
}

static inline u32
hash_32(u32 val, unsigned int bits)
{
	return (__hash_32(val) >> (32 - bits));
}

static inline u32
hash_64(u64 val, unsigned int bits)
{
	return ((u32)((val * GOLDEN_RATIO_64) >> (64 - bits)));
}

#define	hash_long(val, bits)	hash_64((u64)(val), bits)

static inline u32
hash_ptr(const void *ptr, unsigned int bits)
{
	return (hash_long((unsigned long)ptr, bits));
}

static inline u32
hash32_ptr(const void *ptr)
{
	unsigned long val = (unsigned long)ptr;

	return ((u32)(val ^ (val >> 32)));
}

#endif
