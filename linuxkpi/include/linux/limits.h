/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's integer limits (include/linux/limits.h, vdso/limits.h; GPL-2.0;
 * reimplemented). illumos <sys/types.h> already has INT_MAX and friends
 * in some configurations; they are defined here only when missing.
 */
#ifndef _LINUX_LIMITS_H
#define	_LINUX_LIMITS_H

#include <linux/types.h>

#ifndef	USHRT_MAX
#define	USHRT_MAX	((unsigned short)~0U)
#endif
#ifndef	SHRT_MAX
#define	SHRT_MAX	((short)(USHRT_MAX >> 1))
#endif
#ifndef	SHRT_MIN
#define	SHRT_MIN	((short)(-SHRT_MAX - 1))
#endif
#ifndef	INT_MAX
#define	INT_MAX		((int)(~0U >> 1))
#endif
#ifndef	INT_MIN
#define	INT_MIN		(-INT_MAX - 1)
#endif
#ifndef	UINT_MAX
#define	UINT_MAX	(~0U)
#endif
#ifndef	LONG_MAX
#define	LONG_MAX	((long)(~0UL >> 1))
#endif
#ifndef	LONG_MIN
#define	LONG_MIN	(-LONG_MAX - 1)
#endif
#ifndef	ULONG_MAX
#define	ULONG_MAX	(~0UL)
#endif
#ifndef	LLONG_MAX
#define	LLONG_MAX	((long long)(~0ULL >> 1))
#endif
#ifndef	LLONG_MIN
#define	LLONG_MIN	(-LLONG_MAX - 1)
#endif
#ifndef	ULLONG_MAX
#define	ULLONG_MAX	(~0ULL)
#endif
#define	U8_MAX		((u8)~0U)
#define	S8_MAX		((s8)(U8_MAX >> 1))
#define	S8_MIN		((s8)(-S8_MAX - 1))
#define	U16_MAX		((u16)~0U)
#define	S16_MAX		((s16)(U16_MAX >> 1))
#define	S16_MIN		((s16)(-S16_MAX - 1))
#define	U32_MAX		((u32)~0U)
#define	U32_MIN		((u32)0)
#define	S32_MAX		((s32)(U32_MAX >> 1))
#define	S32_MIN		((s32)(-S32_MAX - 1))
#define	U64_MAX		((u64)~0ULL)
#define	S64_MAX		((s64)(U64_MAX >> 1))
#define	S64_MIN		((s64)(-S64_MAX - 1))
#ifndef	SIZE_MAX
#define	SIZE_MAX	(~(size_t)0)
#endif
#ifndef	SSIZE_MAX
#define	SSIZE_MAX	((ssize_t)(SIZE_MAX >> 1))
#endif
#define	PHYS_ADDR_MAX	(~(phys_addr_t)0)
#define	RESOURCE_SIZE_MAX	((resource_size_t)~0)
#define	KTIME_MAX	((s64)~((u64)1 << 63))
#define	KTIME_MIN	(-KTIME_MAX - 1)
#define	KTIME_SEC_MAX	(KTIME_MAX / NSEC_PER_SEC)
#define	TIME64_MAX	((s64)~((u64)1 << 63))
#define	TIME64_MIN	(-TIME64_MAX - 1)

#endif
