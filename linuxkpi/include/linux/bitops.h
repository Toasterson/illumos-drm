/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's bit operations (include/linux/bitops.h, asm/bitops.h,
 * asm-generic/bitops/*; GPL-2.0; reimplemented). The atomic forms use
 * gcc's __atomic builtins (lock-prefixed instructions, full barriers on
 * x86); the double-underscore forms are plain.
 * Bit numbers count from bit 0 of word 0, as on little-endian Linux.
 */
#ifndef _LINUX_BITOPS_H
#define	_LINUX_BITOPS_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/typecheck.h>
#include <asm/barrier.h>

#define	BITS_PER_TYPE(type)	(sizeof (type) * BITS_PER_BYTE)
#define	BITS_TO_LONGS(nr)	(((nr) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define	BITS_TO_U64(nr)		(((nr) + 63) / 64)
#define	BITS_TO_U32(nr)		(((nr) + 31) / 32)
#define	BITS_TO_BYTES(nr)	(((nr) + 7) / 8)

/* Undo the prologue's rename of illumos' ffs() for the Linux one. */
#define	ffs(x)	lkpi_ffs(x)

static inline int
lkpi_ffs(int x)
{
	return (__builtin_ffs(x));
}

/* Linux: index of the last set bit counting from 1, 0 for 0. */
static inline int
fls(unsigned int x)
{
	return (x == 0 ? 0 : 32 - __builtin_clz(x));
}

static inline int
fls64(u64 x)
{
	return (x == 0 ? 0 : 64 - __builtin_clzll(x));
}

static inline unsigned long
__fls(unsigned long w)
{
	return (BITS_PER_LONG - 1 - __builtin_clzl(w));
}

static inline unsigned long
__ffs(unsigned long w)
{
	return ((unsigned long)__builtin_ctzl(w));
}

static inline unsigned long
__ffs64(u64 w)
{
	return ((unsigned long)__builtin_ctzll(w));
}

static inline unsigned long
ffz(unsigned long w)
{
	return (__ffs(~w));
}

static inline unsigned int
fls_long(unsigned long l)
{
	return (fls64(l));
}

/*
 * Population counts with bit arithmetic: __builtin_popcount() without
 * -mpopcnt is a libgcc call, which a kernel module cannot link. Constant
 * arguments fold.
 */
static inline unsigned int
lkpi_hweight64(u64 w)
{
	w = w - ((w >> 1) & 0x5555555555555555ULL);
	w = (w & 0x3333333333333333ULL) + ((w >> 2) & 0x3333333333333333ULL);
	w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
	return ((unsigned int)((w * 0x0101010101010101ULL) >> 56));
}

#define	hweight8(w)	lkpi_hweight64((u8)(w))
#define	hweight16(w)	lkpi_hweight64((u16)(w))
#define	hweight32(w)	lkpi_hweight64((u32)(w))
#define	hweight64(w)	lkpi_hweight64((u64)(w))
#define	hweight_long(w)	((unsigned long)lkpi_hweight64((unsigned long)(w)))

static inline u32
rol32(u32 w, unsigned int s)
{
	return ((w << (s & 31)) | (w >> ((-s) & 31)));
}

static inline u32
ror32(u32 w, unsigned int s)
{
	return ((w >> (s & 31)) | (w << ((-s) & 31)));
}

static inline u64
rol64(u64 w, unsigned int s)
{
	return ((w << (s & 63)) | (w >> ((-s) & 63)));
}

static inline u64
ror64(u64 w, unsigned int s)
{
	return ((w >> (s & 63)) | (w << ((-s) & 63)));
}

static inline s32
sign_extend32(u32 value, int index)
{
	u8 shift = 31 - index;

	return ((s32)(value << shift) >> shift);
}

static inline s64
sign_extend64(u64 value, int index)
{
	u8 shift = 63 - index;

	return ((s64)(value << shift) >> shift);
}

static inline unsigned int
get_count_order(unsigned int count)
{
	if (count == 0)
		return ((unsigned int)-1);
	return (fls(--count));
}

static inline int
get_count_order_long(unsigned long l)
{
	if (l == 0UL)
		return (-1);
	return ((int)fls_long(--l));
}

/* --- atomic bit operations on unsigned long arrays ------------------- */

#define	__lkpi_bitword(nr, addr) \
	((volatile unsigned long *)(addr) + ((nr) / BITS_PER_LONG))
#define	__lkpi_bitmask(nr)	(1UL << ((nr) % BITS_PER_LONG))

static inline void
set_bit(long nr, volatile unsigned long *addr)
{
	(void)__atomic_fetch_or(__lkpi_bitword(nr, addr), __lkpi_bitmask(nr),
	    __ATOMIC_SEQ_CST);
}

static inline void
clear_bit(long nr, volatile unsigned long *addr)
{
	(void)__atomic_fetch_and(__lkpi_bitword(nr, addr), ~__lkpi_bitmask(nr),
	    __ATOMIC_SEQ_CST);
}

static inline void
change_bit(long nr, volatile unsigned long *addr)
{
	(void)__atomic_fetch_xor(__lkpi_bitword(nr, addr), __lkpi_bitmask(nr),
	    __ATOMIC_SEQ_CST);
}

static inline bool
test_and_set_bit(long nr, volatile unsigned long *addr)
{
	unsigned long m = __lkpi_bitmask(nr);

	return ((__atomic_fetch_or(__lkpi_bitword(nr, addr), m,
	    __ATOMIC_SEQ_CST) & m) != 0);
}

static inline bool
test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	unsigned long m = __lkpi_bitmask(nr);

	return ((__atomic_fetch_and(__lkpi_bitword(nr, addr), ~m,
	    __ATOMIC_SEQ_CST) & m) != 0);
}

static inline bool
test_and_change_bit(long nr, volatile unsigned long *addr)
{
	unsigned long m = __lkpi_bitmask(nr);

	return ((__atomic_fetch_xor(__lkpi_bitword(nr, addr), m,
	    __ATOMIC_SEQ_CST) & m) != 0);
}

#define	test_and_set_bit_lock(nr, addr)	test_and_set_bit(nr, addr)

static inline void
clear_bit_unlock(long nr, volatile unsigned long *addr)
{
	barrier();
	clear_bit(nr, addr);
}

#define	__clear_bit_unlock(nr, addr)	clear_bit_unlock(nr, addr)



static inline bool
test_bit(long nr, const volatile unsigned long *addr)
{
	return ((addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1UL);
}

static inline bool
test_bit_acquire(long nr, const volatile unsigned long *addr)
{
	bool r = test_bit(nr, addr);

	barrier();
	return (r);
}

/* --- the non-atomic forms --------------------------------------------- */

static inline void
__set_bit(long nr, volatile unsigned long *addr)
{
	*__lkpi_bitword(nr, addr) |= __lkpi_bitmask(nr);
}

static inline void
__clear_bit(long nr, volatile unsigned long *addr)
{
	*__lkpi_bitword(nr, addr) &= ~__lkpi_bitmask(nr);
}

static inline void
__change_bit(long nr, volatile unsigned long *addr)
{
	*__lkpi_bitword(nr, addr) ^= __lkpi_bitmask(nr);
}

static inline bool
__test_and_set_bit(long nr, volatile unsigned long *addr)
{
	bool o = test_bit(nr, addr);

	__set_bit(nr, addr);
	return (o);
}

static inline bool
__test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	bool o = test_bit(nr, addr);

	__clear_bit(nr, addr);
	return (o);
}

static inline void
__assign_bit(long nr, volatile unsigned long *addr, bool value)
{
	if (value)
		__set_bit(nr, addr);
	else
		__clear_bit(nr, addr);
}

static inline void
assign_bit(long nr, volatile unsigned long *addr, bool value)
{
	if (value)
		set_bit(nr, addr);
	else
		clear_bit(nr, addr);
}

/* --- searching --------------------------------------------------------- */

extern unsigned long _find_next_bit(const unsigned long *addr,
    unsigned long size, unsigned long offset, unsigned long invert);
extern unsigned long _find_last_bit(const unsigned long *addr,
    unsigned long size);

static inline unsigned long
find_next_bit(const unsigned long *addr, unsigned long size,
    unsigned long offset)
{
	return (_find_next_bit(addr, size, offset, 0));
}

static inline unsigned long
find_next_zero_bit(const unsigned long *addr, unsigned long size,
    unsigned long offset)
{
	return (_find_next_bit(addr, size, offset, ~0UL));
}

static inline unsigned long
find_first_bit(const unsigned long *addr, unsigned long size)
{
	return (_find_next_bit(addr, size, 0, 0));
}

static inline unsigned long
find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return (_find_next_bit(addr, size, 0, ~0UL));
}

static inline unsigned long
find_last_bit(const unsigned long *addr, unsigned long size)
{
	return (_find_last_bit(addr, size));
}

#define	for_each_set_bit(bit, addr, size) \
	for ((bit) = find_first_bit((addr), (size)); (bit) < (size); \
	    (bit) = find_next_bit((addr), (size), (bit) + 1))
#define	for_each_set_bit_from(bit, addr, size) \
	for ((bit) = find_next_bit((addr), (size), (bit)); (bit) < (size); \
	    (bit) = find_next_bit((addr), (size), (bit) + 1))
#define	for_each_clear_bit(bit, addr, size) \
	for ((bit) = find_first_zero_bit((addr), (size)); (bit) < (size); \
	    (bit) = find_next_zero_bit((addr), (size), (bit) + 1))


#include <linux/log2.h>

#endif
