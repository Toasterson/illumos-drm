/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's fixed-point logarithms intlog2() and intlog10()
 * (<linux/int_log.h>), computed bit by bit with integer arithmetic (no
 * table, no floating point, no 128-bit helpers from libgcc).
 *
 * log2 is computed in 32.32 fixed point, accurate to about 2^-30, and
 * rounded to 8.24; log10 is that log2 times log10(2) (a 0.64 constant),
 * rounded to 8.24. The error before rounding is far below 2^-25, so exact
 * powers of 2 (intlog2) and of 10 (intlog10) give exact results.
 */

#include <linux/int_log.h>
#include <linux/bug.h>

/* log10(2) * 2^64, rounded. */
#define	LKPI_LOG10_2_Q64	0x4d104d427de7fbccULL

/*
 * log2(value) in 32.32 fixed point, value != 0. The integer part n is
 * the index of the highest set bit. The fraction is that of
 * log2(m), m = value / 2^n in [1, 2), kept in 1.31 fixed point: squaring
 * m doubles log2(m), so each squaring shifts the next fraction bit into
 * the integer position; when the square reaches 2, that bit is 1 and m
 * is halved back into [1, 2). m < 2^32, so m * m fits in 64 bits.
 */
static uint64_t
lkpi_log2_q32(uint32_t value)
{
	uint64_t m, frac = 0;
	int n = 31;
	int i;

	while ((value & (1U << n)) == 0)
		n--;
	m = (uint64_t)value << (31 - n);
	for (i = 31; i >= 0; i--) {
		/* The square, rounded to 1.31 (m in [2^31, 2^32)). */
		m = (m * m + (1ULL << 30)) >> 31;
		if (m >= (1ULL << 32)) {
			m >>= 1;
			frac |= 1ULL << i;
		}
	}
	return (((uint64_t)n << 32) | frac);
}

/*
 * The high 64 bits of the 128-bit product a * b, from 32-bit halves (no
 * 128-bit arithmetic).
 */
static uint64_t
lkpi_mul_hi64(uint64_t a, uint64_t b)
{
	uint64_t a0 = a & 0xffffffffULL, a1 = a >> 32;
	uint64_t b0 = b & 0xffffffffULL, b1 = b >> 32;
	uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	uint64_t mid;

	mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
	return (p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32));
}

unsigned int
intlog2(u32 value)
{
	if (WARN_ON(value == 0))
		return (0);
	/* 32.32 to 8.24, rounded; at most 32 << 24. */
	return ((unsigned int)((lkpi_log2_q32(value) + 0x80) >> 8));
}

unsigned int
intlog10(u32 value)
{
	uint64_t hi;

	if (WARN_ON(value == 0))
		return (0);
	/*
	 * log2 (32.32, below 2^37) times log10(2) (0.64) is a 32.96 number;
	 * its high 64 bits are log10 in 32.32 (the dropped low part only
	 * truncates below 2^-32). Round that to 8.24.
	 */
	hi = lkpi_mul_hi64(lkpi_log2_q32(value), LKPI_LOG10_2_Q64);
	return ((unsigned int)((hi + 0x80) >> 8));
}
