/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's upper_32_bits() family. */
#ifndef _LINUX_WORDPART_H
#define	_LINUX_WORDPART_H
#define	upper_32_bits(n)	((u32)(((n) >> 16) >> 16))
#define	lower_32_bits(n)	((u32)((n) & 0xffffffff))
#define	upper_16_bits(n)	((u16)((n) >> 16))
#define	lower_16_bits(n)	((u16)((n) & 0xffff))
#define	REPEAT_BYTE(x)		((~0ul / 0xff) * (x))
#define	REPEAT_BYTE_U32(x)	lower_32_bits(REPEAT_BYTE(x))
#endif
