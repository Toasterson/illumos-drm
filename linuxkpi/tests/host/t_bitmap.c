/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of <linux/bitops.h> and src/lkpi_bitmap.c against a bool-array
 * reference: set/clear/weight, the searches, the logical operations,
 * the shifts (also in place), regions, zero areas, u32 conversion and
 * the text forms.
 */

#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include "test.h"

#define	NB	300
#define	NW	BITS_TO_LONGS(NB)

static void
ref_to_map(const bool *ref, unsigned long *map, unsigned int n)
{
	unsigned int i;

	memset(map, 0, BITS_TO_LONGS(n) * sizeof (long));
	for (i = 0; i < n; i++) {
		if (ref[i])
			__set_bit(i, map);
	}
}

static bool
map_eq_ref(const unsigned long *map, const bool *ref, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (test_bit(i, map) != ref[i])
			return (false);
	}
	return (true);
}

TEST(bitops)
{
	unsigned long w[2] = { 0, 0 };

	CHECK_EQ(fls(0), 0);
	CHECK_EQ(fls(1), 1);
	CHECK_EQ(fls(0x80000000U), 32);
	CHECK_EQ(fls64(1ULL << 40), 41);
	CHECK_EQ(__ffs(0x10UL), 4);
	CHECK_EQ(__fls(0x10UL), 4);
	CHECK_EQ(ffz(0xffUL), 8);
	CHECK_EQ(ffs(0), 0);
	CHECK_EQ(ffs(8), 4);
	CHECK_EQ(hweight32(0xf0f0f0f0U), 16);
	CHECK_EQ(hweight64(~0ULL), 64);
	CHECK_EQ(hweight8(0x81), 2);
	CHECK_EQ(get_count_order(1), 0);
	CHECK_EQ(get_count_order(5), 3);
	CHECK_EQ(get_count_order(8), 3);
	CHECK_EQ(rol32(0x80000001U, 1), 3);
	CHECK_EQ(ror32(3, 1), 0x80000001U);
	CHECK_EQ(sign_extend32(0x80, 7), -128);
	set_bit(70, w);
	CHECK(test_bit(70, w));
	CHECK(w[1] == (1UL << 6));
	CHECK(test_and_set_bit(70, w));
	CHECK(test_and_clear_bit(70, w));
	CHECK(!test_and_clear_bit(70, w));
	change_bit(3, w);
	CHECK_EQ(w[0], 8);
	__assign_bit(4, w, true);
	CHECK_EQ(w[0], 0x18);
	clear_bit(3, w);
	CHECK_EQ(w[0], 0x10);
	CHECK(!__test_and_set_bit(64, w));
	CHECK(__test_and_clear_bit(64, w));
}

TEST(bitmap_random)
{
	unsigned long long seed = 7;
	unsigned long a[NW], b[NW], d[NW];
	bool ra[NB], rb[NB], rd[NB];
	int round;
	bool ok_set = true, ok_find = true, ok_ops = true, ok_shift = true;

	for (round = 0; round < 400; round++) {
		unsigned int n = 1 + (unsigned int)(t_rand(&seed) % NB);
		unsigned int i, s, len, w;
		unsigned long r;

		for (i = 0; i < NB; i++) {
			ra[i] = (t_rand(&seed) % 3) == 0;
			rb[i] = (t_rand(&seed) % 2) == 0;
		}
		ref_to_map(ra, a, NB);
		ref_to_map(rb, b, NB);

		/* bitmap_set/bitmap_clear on random ranges. */
		s = (unsigned int)(t_rand(&seed) % n);
		len = (unsigned int)(t_rand(&seed) % (n - s + 1));
		bitmap_set(a, s, len);
		for (i = s; i < s + len; i++)
			ra[i] = true;
		if (!map_eq_ref(a, ra, NB))
			ok_set = false;
		s = (unsigned int)(t_rand(&seed) % n);
		len = (unsigned int)(t_rand(&seed) % (n - s + 1));
		bitmap_clear(a, s, len);
		for (i = s; i < s + len; i++)
			ra[i] = false;
		if (!map_eq_ref(a, ra, NB))
			ok_set = false;

		/* Weight and searches within n bits. */
		for (w = 0, i = 0; i < n; i++)
			w += ra[i];
		if (bitmap_weight(a, n) != w)
			ok_find = false;
		s = (unsigned int)(t_rand(&seed) % (n + 2));
		for (r = n, i = s; i < n; i++) {
			if (ra[i]) {
				r = i;
				break;
			}
		}
		if (find_next_bit(a, n, s) != r)
			ok_find = false;
		for (r = n, i = s; i < n; i++) {
			if (!ra[i]) {
				r = i;
				break;
			}
		}
		if (find_next_zero_bit(a, n, s) != r)
			ok_find = false;
		for (r = n, i = n; i-- > 0; ) {
			if (ra[i]) {
				r = i;
				break;
			}
		}
		if (find_last_bit(a, n) != r)
			ok_find = false;
		if (bitmap_empty(a, n) != (w == 0) ||
		    bitmap_full(a, n) != (w == n))
			ok_find = false;

		/* Logical operations. */
		{
			bool any = false, inter = false, sub = true, eq = true;

			for (i = 0; i < n; i++) {
				rd[i] = ra[i] && rb[i];
				any |= rd[i];
				inter |= ra[i] && rb[i];
				if (ra[i] && !rb[i])
					sub = false;
				if (ra[i] != rb[i])
					eq = false;
			}
			if (bitmap_and(d, a, b, n) != any ||
			    !map_eq_ref(d, rd, n))
				ok_ops = false;
			if (bitmap_intersects(a, b, n) != inter ||
			    bitmap_subset(a, b, n) != sub ||
			    bitmap_equal(a, b, n) != eq)
				ok_ops = false;
			any = false;
			for (i = 0; i < n; i++) {
				rd[i] = ra[i] && !rb[i];
				any |= rd[i];
			}
			if (bitmap_andnot(d, a, b, n) != any ||
			    !map_eq_ref(d, rd, n))
				ok_ops = false;
			for (i = 0; i < n; i++)
				rd[i] = ra[i] || rb[i];
			bitmap_or(d, a, b, n);
			if (!map_eq_ref(d, rd, n))
				ok_ops = false;
			for (i = 0; i < n; i++)
				rd[i] = ra[i] != rb[i];
			bitmap_xor(d, a, b, n);
			if (!map_eq_ref(d, rd, n))
				ok_ops = false;
			for (i = 0; i < n; i++)
				rd[i] = !ra[i];
			bitmap_complement(d, a, n);
			if (!map_eq_ref(d, rd, n))
				ok_ops = false;
			bitmap_copy(d, a, n);
			if (!bitmap_equal(d, a, n))
				ok_ops = false;
		}

		/* Shifts, to another map and in place. */
		s = (unsigned int)(t_rand(&seed) % (n + 70));
		for (i = 0; i < n; i++)
			rd[i] = (i >= s) ? ra[i - s] : false;
		bitmap_shift_left(d, a, s, n);
		if (!map_eq_ref(d, rd, n))
			ok_shift = false;
		/* Bits above n in the last word are cleared. */
		if ((n % BITS_PER_LONG) != 0 &&
		    (d[BITS_TO_LONGS(n) - 1] & ~BITMAP_LAST_WORD_MASK(n)) != 0)
			ok_shift = false;
		bitmap_copy(d, a, NB);
		bitmap_shift_left(d, d, s, n);
		if (!map_eq_ref(d, rd, n))
			ok_shift = false;
		for (i = 0; i < n; i++)
			rd[i] = (i + s < n) ? ra[i + s] : false;
		bitmap_shift_right(d, a, s, n);
		if (!map_eq_ref(d, rd, n))
			ok_shift = false;
		bitmap_copy(d, a, NB);
		bitmap_shift_right(d, d, s, n);
		if (!map_eq_ref(d, rd, n))
			ok_shift = false;
	}
	CHECK(ok_set);
	CHECK(ok_find);
	CHECK(ok_ops);
	CHECK(ok_shift);
}

TEST(bitmap_regions)
{
	unsigned long m[4] = { 0 }, *z;
	u32 arr[5] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x5 };
	u32 out[5];
	char *page;
	int i;

	CHECK_EQ(bitmap_find_free_region(m, 64, 3), 0);
	CHECK_EQ(bitmap_find_free_region(m, 64, 3), 8);
	CHECK_EQ(bitmap_weight(m, 64), 16);
	bitmap_release_region(m, 0, 3);
	CHECK_EQ(bitmap_find_free_region(m, 64, 2), 0);
	CHECK_EQ(bitmap_allocate_region(m, 4, 2), 0);
	CHECK_EQ(bitmap_allocate_region(m, 4, 2), -EBUSY);
	CHECK_EQ(bitmap_find_free_region(m, 16, 2), -ENOMEM);
	CHECK_EQ(bitmap_find_free_region(m, 64, 4), 16);

	/* Zero areas with alignment. */
	bitmap_zero(m, 256);
	bitmap_set(m, 0, 5);
	bitmap_set(m, 9, 1);
	CHECK_EQ(bitmap_find_next_zero_area(m, 256, 0, 3, 0), 5);
	CHECK_EQ(bitmap_find_next_zero_area(m, 256, 0, 4, 0), 10);
	CHECK_EQ(bitmap_find_next_zero_area(m, 256, 0, 3, 7), 16);
	CHECK(bitmap_find_next_zero_area(m, 12, 0, 4, 0) > 12 - 4);

	/* u32 arrays, bit 0 of word 0 first. */
	bitmap_from_arr32(m, arr, 132);
	CHECK(m[0] == 0x2222222211111111UL);
	CHECK(m[1] == 0x4444444433333333UL);
	CHECK(m[2] == 0x5UL);
	bitmap_from_arr32(m, arr, 130);
	CHECK(m[2] == 0x1UL);
	memset(out, 0xff, sizeof (out));
	bitmap_to_arr32(out, m, 130);
	CHECK(out[0] == arr[0] && out[3] == arr[3] && out[4] == 1);

	z = bitmap_zalloc(200, GFP_KERNEL);
	CHECK(z != NULL && bitmap_empty(z, 200));
	bitmap_fill(z, 200);
	CHECK(bitmap_full(z, 200));
	CHECK_EQ(bitmap_weight(z, 200), 200);
	CHECK_EQ(z[3], BITMAP_LAST_WORD_MASK(200));
	bitmap_free(z);

	/* The text forms (page-aligned buffer, as sysfs gives). */
	page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	bitmap_zero(m, 256);
	bitmap_set(m, 0, 4);
	bitmap_set(m, 8, 1);
	bitmap_set(m, 62, 4);
	i = bitmap_print_to_pagebuf(true, page, m, 100);
	CHECK_STR(page, "0-3,8,62-65\n");
	CHECK_EQ(i, (int)strlen("0-3,8,62-65\n"));
	(void) bitmap_print_to_pagebuf(false, page, m, 72);
	CHECK_STR(page, "03,c0000000,0000010f\n");
	free(page);
}
