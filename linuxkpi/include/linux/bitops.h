/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's bit operations on unsigned long words, for the carried helpers
 * (carried/): BIT(), BIT_ULL(), the non-atomic __set_bit()/__clear_bit(),
 * test_bit(), the atomic set_bit()/clear_bit() and clear_bit_unlock()
 * (atomic, with release ordering). The atomic forms are GCC's __atomic
 * built-ins, which compile to locked instructions on amd64 (no library
 * call), so this header is the same in the kernel and in host tests.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's include/linux/bitops.h
 * and include/asm-generic/bitops/ (GPL-2.0). This file is a
 * reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_BITOPS_H
#define	_LINUXKPI_LINUX_BITOPS_H

#include <linux/types.h>

#define	BITS_PER_LONG	(8 * (int)sizeof (long))
#define	BIT(nr)		(1UL << (nr))
#define	BIT_ULL(nr)	(1ULL << (nr))

#define	__lkpi_bit_word(nr, addr) \
	((volatile unsigned long *)(addr) + (unsigned long)(nr) / BITS_PER_LONG)
#define	__lkpi_bit_mask(nr)	(1UL << ((unsigned long)(nr) % BITS_PER_LONG))

static inline void
__set_bit(long nr, volatile unsigned long *addr)
{
	*__lkpi_bit_word(nr, addr) |= __lkpi_bit_mask(nr);
}

static inline void
__clear_bit(long nr, volatile unsigned long *addr)
{
	*__lkpi_bit_word(nr, addr) &= ~__lkpi_bit_mask(nr);
}

static inline bool
test_bit(long nr, const volatile unsigned long *addr)
{
	return ((*__lkpi_bit_word(nr, addr) & __lkpi_bit_mask(nr)) != 0);
}

static inline void
set_bit(long nr, volatile unsigned long *addr)
{
	(void) __atomic_fetch_or(__lkpi_bit_word(nr, addr),
	    __lkpi_bit_mask(nr), __ATOMIC_RELAXED);
}

static inline void
clear_bit(long nr, volatile unsigned long *addr)
{
	(void) __atomic_fetch_and(__lkpi_bit_word(nr, addr),
	    ~__lkpi_bit_mask(nr), __ATOMIC_RELAXED);
}

/* Clears the bit atomically after every earlier access (release). */
static inline void
clear_bit_unlock(long nr, volatile unsigned long *addr)
{
	(void) __atomic_fetch_and(__lkpi_bit_word(nr, addr),
	    ~__lkpi_bit_mask(nr), __ATOMIC_RELEASE);
}

#endif	/* _LINUXKPI_LINUX_BITOPS_H */
