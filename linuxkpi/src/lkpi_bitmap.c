/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's bit searches (<linux/bitops.h>) and bitmap operations
 * (<linux/bitmap.h>), written from their documented semantics. A bitmap
 * is an array of unsigned long, bit 0 of word 0 first; bits at and above
 * nbits in the last word are ignored on input and, where Linux does so,
 * left zero on output. Population counts use bit arithmetic (no libgcc
 * __popcountdi2 in the kernel).
 */

#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/errno.h>

#define	WORD(n)		((n) / BITS_PER_LONG)
#define	FULL_WORDS(n)	((n) / BITS_PER_LONG)

static inline unsigned int
lkpi_popcount(unsigned long w)
{
	w = w - ((w >> 1) & 0x5555555555555555UL);
	w = (w & 0x3333333333333333UL) + ((w >> 2) & 0x3333333333333333UL);
	w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0fUL;
	return ((unsigned int)((w * 0x0101010101010101UL) >> 56));
}

/*
 * The first bit at or after `offset` that is set in addr[] ^ invert
 * (invert 0: set bits, ~0UL: clear bits), or `size` if there is none.
 */
unsigned long
_find_next_bit(const unsigned long *addr, unsigned long size,
    unsigned long offset, unsigned long invert)
{
	unsigned long w, i;

	if (offset >= size)
		return (size);
	i = WORD(offset);
	w = (addr[i] ^ invert) & BITMAP_FIRST_WORD_MASK(offset);
	for (;;) {
		if (w != 0) {
			offset = i * BITS_PER_LONG + __ffs(w);
			return (offset < size ? offset : size);
		}
		if (++i * BITS_PER_LONG >= size)
			return (size);
		w = addr[i] ^ invert;
	}
}

unsigned long
_find_last_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long w, i;

	if (size == 0)
		return (0);
	i = WORD(size - 1);
	w = addr[i] & BITMAP_LAST_WORD_MASK(size);
	for (;;) {
		if (w != 0)
			return (i * BITS_PER_LONG + __fls(w));
		if (i == 0)
			return (size);
		w = addr[--i];
	}
}

unsigned long *
bitmap_alloc(unsigned int nbits, gfp_t flags)
{
	return (kmalloc(BITS_TO_ARR(nbits), flags));
}

unsigned long *
bitmap_zalloc(unsigned int nbits, gfp_t flags)
{
	return (kmalloc(BITS_TO_ARR(nbits), flags | __GFP_ZERO));
}

void
bitmap_free(const unsigned long *bitmap)
{
	kfree(bitmap);
}

void
bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
	unsigned long *p = map + WORD(start);
	unsigned int end = start + nbits;
	unsigned long mask = BITMAP_FIRST_WORD_MASK(start);
	unsigned int left = BITS_PER_LONG - (start % BITS_PER_LONG);

	if (nbits == 0)
		return;
	while (nbits >= left) {
		*p++ |= mask;
		nbits -= left;
		left = BITS_PER_LONG;
		mask = ~0UL;
	}
	if (nbits != 0)
		*p |= mask & BITMAP_LAST_WORD_MASK(end);
}

void
bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
	unsigned long *p = map + WORD(start);
	unsigned int end = start + nbits;
	unsigned long mask = BITMAP_FIRST_WORD_MASK(start);
	unsigned int left = BITS_PER_LONG - (start % BITS_PER_LONG);

	if (nbits == 0)
		return;
	while (nbits >= left) {
		*p++ &= ~mask;
		nbits -= left;
		left = BITS_PER_LONG;
		mask = ~0UL;
	}
	if (nbits != 0)
		*p &= ~(mask & BITMAP_LAST_WORD_MASK(end));
}

unsigned int
bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	unsigned int i, w = 0;

	for (i = 0; i < FULL_WORDS(nbits); i++)
		w += lkpi_popcount(src[i]);
	if (nbits % BITS_PER_LONG)
		w += lkpi_popcount(src[i] & BITMAP_LAST_WORD_MASK(nbits));
	return (w);
}

bool
bitmap_equal(const unsigned long *a, const unsigned long *b,
    unsigned int nbits)
{
	unsigned int i;

	for (i = 0; i < FULL_WORDS(nbits); i++) {
		if (a[i] != b[i])
			return (false);
	}
	if (nbits % BITS_PER_LONG)
		return (((a[i] ^ b[i]) & BITMAP_LAST_WORD_MASK(nbits)) == 0);
	return (true);
}

/* Every bit of a is also set in b. */
bool
bitmap_subset(const unsigned long *a, const unsigned long *b,
    unsigned int nbits)
{
	unsigned int i;

	for (i = 0; i < FULL_WORDS(nbits); i++) {
		if (a[i] & ~b[i])
			return (false);
	}
	if (nbits % BITS_PER_LONG)
		return ((a[i] & ~b[i] & BITMAP_LAST_WORD_MASK(nbits)) == 0);
	return (true);
}

bool
bitmap_intersects(const unsigned long *a, const unsigned long *b,
    unsigned int nbits)
{
	unsigned int i;

	for (i = 0; i < FULL_WORDS(nbits); i++) {
		if (a[i] & b[i])
			return (true);
	}
	if (nbits % BITS_PER_LONG)
		return ((a[i] & b[i] & BITMAP_LAST_WORD_MASK(nbits)) != 0);
	return (false);
}

/* dst = a & b; whether dst has any bit set. */
bool
bitmap_and(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits)
{
	unsigned int i, n = BITS_TO_LONGS(nbits);
	unsigned long any = 0;

	for (i = 0; i < n; i++) {
		dst[i] = a[i] & b[i];
		if (i == n - 1 && (nbits % BITS_PER_LONG))
			any |= dst[i] & BITMAP_LAST_WORD_MASK(nbits);
		else
			any |= dst[i];
	}
	return (any != 0);
}

void
bitmap_or(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits)
{
	unsigned int i;

	for (i = 0; i < BITS_TO_LONGS(nbits); i++)
		dst[i] = a[i] | b[i];
}

void
bitmap_xor(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits)
{
	unsigned int i;

	for (i = 0; i < BITS_TO_LONGS(nbits); i++)
		dst[i] = a[i] ^ b[i];
}

/* dst = a & ~b; whether dst has any bit set. */
bool
bitmap_andnot(unsigned long *dst, const unsigned long *a,
    const unsigned long *b, unsigned int nbits)
{
	unsigned int i, n = BITS_TO_LONGS(nbits);
	unsigned long any = 0;

	for (i = 0; i < n; i++) {
		dst[i] = a[i] & ~b[i];
		if (i == n - 1 && (nbits % BITS_PER_LONG))
			any |= dst[i] & BITMAP_LAST_WORD_MASK(nbits);
		else
			any |= dst[i];
	}
	return (any != 0);
}

void
bitmap_complement(unsigned long *dst, const unsigned long *src,
    unsigned int nbits)
{
	unsigned int i;

	for (i = 0; i < BITS_TO_LONGS(nbits); i++)
		dst[i] = ~src[i];
}

/*
 * dst = src << shift, bits shifted beyond nbits dropped and the last word
 * cleared above nbits. Works in place (dst == src): words are written
 * from the top down, each from source words at or below it.
 */
void
bitmap_shift_left(unsigned long *dst, const unsigned long *src,
    unsigned int shift, unsigned int nbits)
{
	unsigned int n = BITS_TO_LONGS(nbits);
	unsigned int off = shift / BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	unsigned int k;

	if (n == 0)
		return;
	for (k = n; k-- > 0; ) {
		unsigned long lo = 0, hi = 0;

		if (k >= off) {
			hi = src[k - off];
			if (rem != 0 && k >= off + 1)
				lo = src[k - off - 1];
		}
		dst[k] = rem != 0 ? (hi << rem) | (lo >> (BITS_PER_LONG - rem)) :
		    hi;
	}
	if (nbits % BITS_PER_LONG)
		dst[n - 1] &= BITMAP_LAST_WORD_MASK(nbits);
}

/*
 * dst = src >> shift over nbits bits (the bits above nbits of src are
 * ignored). Works in place: words are written from the bottom up.
 */
void
bitmap_shift_right(unsigned long *dst, const unsigned long *src,
    unsigned int shift, unsigned int nbits)
{
	unsigned int n = BITS_TO_LONGS(nbits);
	unsigned int off = shift / BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	unsigned long last_mask = BITMAP_LAST_WORD_MASK(nbits);
	unsigned int k;

	for (k = 0; k < n; k++) {
		unsigned long lo = 0, hi = 0;

		if (k + off < n) {
			lo = src[k + off];
			if (k + off == n - 1)
				lo &= last_mask;
			if (rem != 0 && k + off + 1 < n) {
				hi = src[k + off + 1];
				if (k + off + 1 == n - 1)
					hi &= last_mask;
			}
		}
		dst[k] = rem != 0 ? (lo >> rem) | (hi << (BITS_PER_LONG - rem)) :
		    lo;
	}
}

/*
 * The first index >= start of a run of nr clear bits whose index is
 * aligned to align_mask + 1; a value > size - nr if there is none.
 */
unsigned long
bitmap_find_next_zero_area(unsigned long *map, unsigned long size,
    unsigned long start, unsigned int nr, unsigned long align_mask)
{
	unsigned long index, end, i;

	for (;;) {
		index = find_next_zero_bit(map, size, start);
		index = (index + align_mask) & ~align_mask;
		end = index + nr;
		if (end > size)
			return (end);
		i = find_next_bit(map, end, index);
		if (i >= end)
			return (index);
		start = i + 1;
	}
}

static bool
lkpi_region_free(const unsigned long *map, unsigned int pos,
    unsigned int len)
{
	return (find_next_bit(map, pos + len, pos) >= pos + len);
}

/*
 * Allocates a free aligned region of 2^order bits; its position or
 * -ENOMEM.
 */
int
bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order)
{
	unsigned int len, pos;

	if (order < 0 || order >= 31)
		return (-EINVAL);
	len = 1U << order;
	for (pos = 0; pos + len <= bits && pos + len > pos; pos += len) {
		if (lkpi_region_free(bitmap, pos, len)) {
			bitmap_set(bitmap, pos, len);
			return ((int)pos);
		}
	}
	return (-ENOMEM);
}

void
bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order)
{
	if (order < 0 || order >= 31)
		return;
	bitmap_clear(bitmap, pos, 1U << order);
}

int
bitmap_allocate_region(unsigned long *bitmap, unsigned int pos, int order)
{
	unsigned int len;

	if (order < 0 || order >= 31)
		return (-EINVAL);
	len = 1U << order;
	if (!lkpi_region_free(bitmap, pos, len))
		return (-EBUSY);
	bitmap_set(bitmap, pos, len);
	return (0);
}

void
bitmap_from_arr32(unsigned long *bitmap, const u32 *buf, unsigned int nbits)
{
	unsigned int i, n = BITS_TO_U32(nbits);

	for (i = 0; i < n; i++) {
		if ((i & 1) == 0)
			bitmap[i / 2] = buf[i];
		else
			bitmap[i / 2] |= (unsigned long)buf[i] << 32;
	}
	if (nbits % BITS_PER_LONG)
		bitmap[BITS_TO_LONGS(nbits) - 1] &= BITMAP_LAST_WORD_MASK(nbits);
}

void
bitmap_to_arr32(u32 *buf, const unsigned long *bitmap, unsigned int nbits)
{
	unsigned int i, n = BITS_TO_U32(nbits);

	for (i = 0; i < n; i++) {
		buf[i] = (u32)(bitmap[i / 2] >> ((i & 1) * 32));
	}
	if (nbits % 32)
		buf[n - 1] &= (u32)(~0U >> (32 - nbits % 32));
}

/*
 * The bitmap as text into a page-sized buffer, with a newline: a list of
 * ranges ("0-3,8") or, as Linux's "%*pb", 32-bit hex words separated by
 * commas, most significant first. The length written.
 */
int
bitmap_print_to_pagebuf(bool list, char *buf, const unsigned long *maskp,
    int nmaskbits)
{
	size_t size = PAGE_SIZE - ((uintptr_t)buf & (PAGE_SIZE - 1));
	unsigned int nbits = nmaskbits < 0 ? 0 : (unsigned int)nmaskbits;
	int len = 0;

	if (list) {
		unsigned long a, b;
		bool first = true;

		for (a = find_first_bit(maskp, nbits); a < nbits;
		    a = find_next_bit(maskp, nbits, b + 1)) {
			b = find_next_zero_bit(maskp, nbits, a + 1) - 1;
			if (b > a)
				len += lkpi_scnprintf(buf + len, size - len,
				    "%s%lu-%lu", first ? "" : ",", a, b);
			else
				len += lkpi_scnprintf(buf + len, size - len,
				    "%s%lu", first ? "" : ",", a);
			first = false;
		}
	} else {
		int nwords = (int)((nbits + 31) / 32), i;

		for (i = nwords - 1; i >= 0; i--) {
			u32 w = (u32)(maskp[i / 2] >> ((i & 1) * 32));
			unsigned int bitsl = nbits - (unsigned int)i * 32;
			int digits = 8;

			if (bitsl < 32) {
				w &= (u32)(~0U >> (32 - bitsl));
				if (i == nwords - 1)
					digits = (int)((bitsl + 3) / 4);
			}
			len += lkpi_scnprintf(buf + len, size - len, "%0*x%s",
			    digits, w, i != 0 ? "," : "");
		}
	}
	len += lkpi_scnprintf(buf + len, size - len, "\n");
	return (len);
}
