/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Self-tests of the carried drm_cache (Linux v7.2
 * drivers/gpu/drm/drm_cache.c, drm_clflush_virt_range() only; see
 * carried/drm_cache/patches/0001-*), run in the kernel by
 * drm_helpers_test against misc/drm_helpers and on x86-64 hosts by
 * carried/host-test.sh.
 *
 * Linux has no KUnit test for drm_cache. A cache write-back has no effect
 * a CPU can observe on coherent memory, so these tests check what can be
 * checked: every range shape (empty, one byte, within and across lines
 * and pages, misaligned, the end of a buffer) is flushed without a fault
 * and leaves the memory's contents as they were, also while four threads
 * flush overlapping ranges of one buffer and write their own (the
 * function reads only the processor facts set at module load). Whether
 * the GPU sees the data is i915's test, on the hardware.
 */

#include <linux/kernel.h>
#include <drm/drm_cache.h>

#include "selftest.h"

#define	ST_CACHE_BUF	(3 * 4096 + 192)
#define	ST_CACHE_WORKERS	4

static uint8_t
st_pattern(size_t i, unsigned int round)
{
	return ((uint8_t)(i * 31 + 7 + round * 13));
}

static void
st_fill(uint8_t *buf, size_t len, unsigned int round)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = st_pattern(i, round);
}

static bool
st_intact(const uint8_t *buf, size_t len, unsigned int round)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] != st_pattern(i, round))
			return (false);
	}
	return (true);
}

static void
st_cache_ranges(struct st_ctx *ctx)
{
	static const struct {
		size_t off, len;
	} r[] = {
		{ 0, 0 }, { 0, 1 }, { 1, 1 }, { 63, 2 }, { 64, 64 },
		{ 65, 63 }, { 5, 4096 }, { 4095, 2 }, { 100, 3 * 4096 - 100 },
		{ 0, ST_CACHE_BUF }, { ST_CACHE_BUF - 1, 1 },
		{ 4096 - 32, 64 }, { 2 * 4096, 4096 + 192 },
	};
	uint8_t *buf;
	unsigned int i, round;

	buf = st_zalloc(ST_CACHE_BUF);
	if (!ST_CHECK(ctx, buf != NULL, "no memory"))
		return;
	for (round = 0; round < 4 * ctx->scale; round++) {
		st_fill(buf, ST_CACHE_BUF, round);
		for (i = 0; i < ARRAY_SIZE(r); i++) {
			drm_clflush_virt_range(buf + r[i].off, r[i].len);
			ST_CHECK(ctx, st_intact(buf, ST_CACHE_BUF, round),
			    "round %u: contents changed after flushing "
			    "[%lu + %lu]", round, (unsigned long)r[i].off,
			    (unsigned long)r[i].len);
		}
		/* Random ranges inside the buffer. */
		for (i = 0; i < 64; i++) {
			size_t off = (size_t)st_rand_below(ctx, ST_CACHE_BUF);
			size_t len = (size_t)st_rand_below(ctx,
			    ST_CACHE_BUF - off + 1);

			drm_clflush_virt_range(buf + off, len);
		}
		ST_CHECK(ctx, st_intact(buf, ST_CACHE_BUF, round),
		    "round %u: contents changed after random flushes", round);
	}
	st_free(buf, ST_CACHE_BUF);
}

struct st_cache_worker {
	struct st_ctx ctx;
	uint8_t *shared;	/* read and flushed by everyone */
	uint8_t *own;		/* written and flushed by this worker */
};

static void
st_cache_worker_run(void *arg)
{
	struct st_cache_worker *w = arg;
	unsigned int round;

	for (round = 0; round < 200 * w->ctx.scale; round++) {
		size_t off = (size_t)st_rand_below(&w->ctx, ST_CACHE_BUF);
		size_t len = (size_t)st_rand_below(&w->ctx,
		    ST_CACHE_BUF - off + 1);

		st_fill(w->own, 4096, round);
		drm_clflush_virt_range(w->own, 4096);
		drm_clflush_virt_range(w->shared + off, len);
		ST_CHECK(&w->ctx, st_intact(w->own, 4096, round),
		    "worker buffer changed in round %u", round);
	}
	ST_CHECK(&w->ctx, st_intact(w->shared, ST_CACHE_BUF, 0),
	    "shared buffer changed");
}

static void
st_cache_parallel(struct st_ctx *ctx)
{
	struct st_cache_worker *w;
	void *args[ST_CACHE_WORKERS];
	uint8_t *shared;
	size_t sz = ST_CACHE_WORKERS * sizeof (*w);
	unsigned int i;

	w = st_zalloc(sz);
	shared = st_zalloc(ST_CACHE_BUF);
	if (!ST_CHECK(ctx, w != NULL && shared != NULL, "no memory"))
		goto out;
	st_fill(shared, ST_CACHE_BUF, 0);
	for (i = 0; i < ST_CACHE_WORKERS; i++) {
		w[i].ctx.test = ctx->test;
		w[i].ctx.scale = ctx->scale;
		st_seed(&w[i].ctx, st_rand(ctx) | 1);
		w[i].shared = shared;
		w[i].own = st_zalloc(4096);
		args[i] = &w[i];
	}
	for (i = 0; i < ST_CACHE_WORKERS; i++) {
		if (!ST_CHECK(ctx, w[i].own != NULL, "no memory"))
			goto out;
	}
	if (ST_CHECK(ctx, st_run_parallel(st_cache_worker_run, args,
	    ST_CACHE_WORKERS) == 0, "could not start %u threads",
	    ST_CACHE_WORKERS)) {
		for (i = 0; i < ST_CACHE_WORKERS; i++) {
			ctx->checks_passed += w[i].ctx.checks_passed;
			ctx->checks_failed += w[i].ctx.checks_failed;
		}
	}
out:
	if (w != NULL) {
		for (i = 0; i < ST_CACHE_WORKERS; i++) {
			if (w[i].own != NULL)
				st_free(w[i].own, 4096);
		}
		st_free(w, sz);
	}
	if (shared != NULL)
		st_free(shared, ST_CACHE_BUF);
}

const struct st_test drm_cache_selftests[] = {
	{ "clflush_ranges", st_cache_ranges, 0 },
	{ "clflush_parallel", st_cache_parallel, 0 },
};
const unsigned int drm_cache_nselftests = ARRAY_SIZE(drm_cache_selftests);
