/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's power-of-two test for the carried helpers (carried/).
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the name and meaning follow Linux v7.2's include/linux/log2.h
 * (GPL-2.0-or-later). This file is a reimplementation, not a copy
 * (decision D7).
 */

#ifndef _LINUXKPI_LINUX_LOG2_H
#define	_LINUXKPI_LINUX_LOG2_H

#include <linux/types.h>

/* True when n is a power of two; 0 is not. */
static inline bool
is_power_of_2(unsigned long n)
{
	return (n != 0 && (n & (n - 1)) == 0);
}

#endif	/* _LINUXKPI_LINUX_LOG2_H */
