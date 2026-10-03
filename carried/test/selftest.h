/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The self-test harness of the carried helpers (carried/): the same test
 * sources run in the kernel, in the test driver drm_helpers_test at attach
 * (carried/test/drm_helpers_test.c, selftest_kernel.c), and in a userland
 * program on any host (carried/test/host/selftest_host.c,
 * carried/host-test.sh).
 *
 * A test is a function that makes checks with ST_CHECK(); a failed check
 * is counted and logged (the first few per test), the test goes on. The
 * environment provides allocation, failure reporting and running a
 * function on several threads at once.
 */

#ifndef _CARRIED_SELFTEST_H
#define	_CARRIED_SELFTEST_H

#include <linux/types.h>

#define	ST_MAX_LOGGED	8	/* failures logged per test */

struct st_ctx {
	const char *test;		/* the running test */
	uint64_t checks_passed;
	uint64_t checks_failed;
	uint32_t logged;		/* failures logged in this test */
	uint32_t scale;			/* iteration multiplier, >= 1 */
	uint64_t rng;			/* xorshift64* state, never 0 */
};

#define	ST_HOST_ONLY	0x1	/* needs a symbol the module does not export */

struct st_test {
	const char *name;
	void (*fn)(struct st_ctx *ctx);
	unsigned int flags;
};

/*
 * Environment functions. st_fail() counts a failed check and logs it
 * unless ST_MAX_LOGGED failures were logged in this test already.
 * st_zalloc() returns zeroed memory or NULL (the kernel's never fails: it
 * sleeps). st_run_parallel() runs fn(args[i]) for every i < n on n threads
 * at once and returns when all have returned: 0, or -1 when not every
 * thread could be started (those that started have returned).
 */
extern void st_fail(struct st_ctx *ctx, int line, const char *fmt, ...)
    __attribute__((__format__(__printf__, 3, 4)));
extern void *st_zalloc(size_t size);
extern void st_free(void *ptr, size_t size);
extern int st_run_parallel(void (*fn)(void *), void **args, unsigned int n);

#define	ST_CHECK(ctx, cond, ...)					\
	((cond) ? ((ctx)->checks_passed++, true) :			\
	    (st_fail((ctx), __LINE__, __VA_ARGS__), false))

/* xorshift64* (Vigna, 2016): a fast, deterministic sequence per seed. */
static inline uint64_t
st_rand(struct st_ctx *ctx)
{
	uint64_t x = ctx->rng;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	ctx->rng = x;
	return (x * 0x2545f4914f6cdd1dULL);
}

/* A number below n (n > 0). */
static inline uint64_t
st_rand_below(struct st_ctx *ctx, uint64_t n)
{
	return (st_rand(ctx) % n);
}

static inline void
st_seed(struct st_ctx *ctx, uint64_t seed)
{
	ctx->rng = seed != 0 ? seed : 0x9e3779b97f4a7c15ULL;
}

/* The tests of carried/drm_mm/tests/drm_mm_selftest.c. */
extern const struct st_test drm_mm_selftests[];
extern const unsigned int drm_mm_nselftests;

#endif	/* _CARRIED_SELFTEST_H */
