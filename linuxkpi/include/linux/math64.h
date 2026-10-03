/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's 64-bit division helpers for the carried helpers (carried/). On
 * amd64 they are plain C division.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's
 * include/linux/math64.h (GPL-2.0). This file is a reimplementation, not
 * a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_MATH64_H
#define	_LINUXKPI_LINUX_MATH64_H

#include <linux/types.h>

/* Quotient of dividend / divisor; the remainder goes to *remainder. */
static inline u64
div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return (dividend / divisor);
}

static inline u64
div64_u64(u64 dividend, u64 divisor)
{
	return (dividend / divisor);
}

static inline u64
div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	*remainder = (u32)(dividend % divisor);
	return (dividend / divisor);
}

static inline u64
div_u64(u64 dividend, u32 divisor)
{
	return (dividend / divisor);
}

static inline s64
div_s64(s64 dividend, s32 divisor)
{
	return (dividend / divisor);
}

#endif	/* _LINUXKPI_LINUX_MATH64_H */
