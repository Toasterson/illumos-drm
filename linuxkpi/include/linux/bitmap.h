/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's bitmaps (include/linux/bitmap.h, GPL-2.0; reimplemented in
 * src/lkpi_bitmap.c). Bitmaps are arrays of unsigned long, bit 0 of word
 * 0 first.
 */
#ifndef _LINUX_BITMAP_H
#define	_LINUX_BITMAP_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/gfp.h>

#define	BITMAP_FIRST_WORD_MASK(start)	(~0UL << ((start) & (BITS_PER_LONG - 1)))
#define	BITMAP_LAST_WORD_MASK(nbits)	(~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

extern unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags);
extern unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags);
extern void bitmap_free(const unsigned long *bitmap);
extern void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits);
extern void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits);
extern unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits);
extern bool bitmap_equal(const unsigned long *a, const unsigned long *b,
    unsigned int nbits);
extern bool bitmap_subset(const unsigned long *a, const unsigned long *b,
    unsigned int nbits);
extern bool bitmap_intersects(const unsigned long *a, const unsigned long *b,
    unsigned int nbits);
extern bool bitmap_and(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits);
extern void bitmap_or(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits);
extern void bitmap_xor(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits);
extern bool bitmap_andnot(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits);
extern void bitmap_complement(unsigned long *dst, const unsigned long *src,
    unsigned int nbits);
extern void bitmap_shift_left(unsigned long *dst, const unsigned long *src,
    unsigned int shift, unsigned int nbits);
extern void bitmap_shift_right(unsigned long *dst, const unsigned long *src,
    unsigned int shift, unsigned int nbits);
extern unsigned long bitmap_find_next_zero_area(unsigned long *map,
    unsigned long size, unsigned long start, unsigned int nr,
    unsigned long align_mask);
extern int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits,
    int order);
extern void bitmap_release_region(unsigned long *bitmap, unsigned int pos,
    int order);
extern int bitmap_allocate_region(unsigned long *bitmap, unsigned int pos,
    int order);
extern void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf,
    unsigned int nbits);
extern void bitmap_to_arr32(u32 *buf, const unsigned long *bitmap,
    unsigned int nbits);
extern int bitmap_print_to_pagebuf(bool list, char *buf,
    const unsigned long *maskp, int nmaskbits);

#define	BITS_TO_ARR(nbits)	(BITS_TO_LONGS(nbits) * sizeof (unsigned long))

static inline void
bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	memset(dst, 0, BITS_TO_ARR(nbits));
}

static inline void
bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	memset(dst, 0xff, BITS_TO_ARR(nbits));
	if (nbits % BITS_PER_LONG)
		dst[nbits / BITS_PER_LONG] &= BITMAP_LAST_WORD_MASK(nbits);
}

static inline void
bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	memcpy(dst, src, BITS_TO_ARR(nbits));
}

static inline bool
bitmap_empty(const unsigned long *src, unsigned int nbits)
{
	return (find_first_bit(src, nbits) >= nbits);
}

static inline bool
bitmap_full(const unsigned long *src, unsigned int nbits)
{
	return (find_first_zero_bit(src, nbits) >= nbits);
}

#define	bitmap_from_arr64(b, buf, n)	memcpy((b), (buf), BITS_TO_ARR(n))
#define	bitmap_to_arr64(buf, b, n)	memcpy((buf), (b), BITS_TO_ARR(n))

#endif
