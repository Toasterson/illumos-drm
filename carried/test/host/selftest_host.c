/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The host environment of the carried helpers' self-tests
 * (carried/test/selftest.h) and the test program's main(): runs the same
 * drm_mm tests as the kernel's drm_helpers_test (and on x86-64 hosts its
 * drm_cache tests), the host-only tests
 * (drm_mm_host_test.c: functions misc/drm_helpers does not export) and the
 * red-black and interval tree tests of linuxkpi (rbtree_test.c).
 *
 *	selftest_host [-s seed] [-x scale] [test-name ...]
 *
 * Exits 0 when every check passed. Built by carried/host-test.sh.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "selftest.h"

#ifdef ST_HAVE_DRM_CACHE
#include <asm/processor.h>
#endif

unsigned long lkpi_host_warnings;

extern const struct st_test drm_mm_host_tests[];
extern const unsigned int drm_mm_nhost_tests;
extern const struct st_test rbtree_tests[];
extern const unsigned int rbtree_ntests;

static pthread_mutex_t st_log_lock = PTHREAD_MUTEX_INITIALIZER;

void
st_fail(struct st_ctx *ctx, int line, const char *fmt, ...)
{
	va_list ap;

	ctx->checks_failed++;
	if (ctx->logged++ >= ST_MAX_LOGGED)
		return;
	(void) pthread_mutex_lock(&st_log_lock);
	(void) fprintf(stderr, "FAIL %s (line %d): ", ctx->test, line);
	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fputc('\n', stderr);
	(void) pthread_mutex_unlock(&st_log_lock);
}

void *
st_zalloc(size_t size)
{
	return (calloc(1, size));
}

void
st_free(void *ptr, size_t size)
{
	(void) size;
	free(ptr);
}

struct st_thread {
	pthread_t tid;
	void (*fn)(void *);
	void *arg;
};

static void *
st_thread_main(void *arg)
{
	struct st_thread *t = arg;

	t->fn(t->arg);
	return (NULL);
}

int
st_run_parallel(void (*fn)(void *), void **args, unsigned int n)
{
	struct st_thread *t = calloc(n, sizeof (*t));
	unsigned int i, started = 0;
	int ret = 0;

	if (t == NULL)
		return (-1);
	for (i = 0; i < n; i++) {
		t[i].fn = fn;
		t[i].arg = args[i];
	}
	for (i = 0; i < n; i++) {
		if (pthread_create(&t[i].tid, NULL, st_thread_main, &t[i]) != 0) {
			ret = -1;
			break;
		}
		started++;
	}
	for (i = 0; i < started; i++)
		(void) pthread_join(t[i].tid, NULL);
	free(t);
	return (ret);
}

static int
st_selected(const char *name, int argc, char **argv, int first)
{
	int i;

	if (first >= argc)
		return (1);
	for (i = first; i < argc; i++) {
		if (strcmp(argv[i], name) == 0)
			return (1);
	}
	return (0);
}

static void
st_run_suite(const char *suite, const struct st_test *tests, unsigned int n,
    uint64_t seed, uint32_t scale, int argc, char **argv, int first,
    unsigned int *passed, unsigned int *failed, uint64_t *checks)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct st_ctx ctx;

		if (!st_selected(tests[i].name, argc, argv, first))
			continue;
		memset(&ctx, 0, sizeof (ctx));
		ctx.test = tests[i].name;
		ctx.scale = scale;
		st_seed(&ctx, seed + i);
		tests[i].fn(&ctx);
		*checks += ctx.checks_passed + ctx.checks_failed;
		if (ctx.checks_failed == 0 && ctx.checks_passed > 0) {
			(*passed)++;
			(void) printf("ok   %s/%s: %llu checks\n", suite,
			    tests[i].name, (unsigned long long)ctx.checks_passed);
		} else {
			(*failed)++;
			(void) printf("FAIL %s/%s: %llu of %llu checks failed\n",
			    suite, tests[i].name,
			    (unsigned long long)ctx.checks_failed,
			    (unsigned long long)(ctx.checks_passed +
			    ctx.checks_failed));
		}
	}
}

int
main(int argc, char **argv)
{
	uint64_t seed = 20261004, checks = 0;
	uint32_t scale = 1;
	unsigned int passed = 0, failed = 0;
	int first = 1;

	while (first + 1 < argc && argv[first][0] == '-') {
		if (strcmp(argv[first], "-s") == 0)
			seed = strtoull(argv[first + 1], NULL, 0);
		else if (strcmp(argv[first], "-x") == 0)
			scale = (uint32_t)strtoul(argv[first + 1], NULL, 0);
		else
			break;
		first += 2;
	}
	if (scale == 0)
		scale = 1;
	(void) printf("seed %llu, scale %u\n", (unsigned long long)seed, scale);
	st_run_suite("drm_mm", drm_mm_selftests, drm_mm_nselftests, seed, scale,
	    argc, argv, first, &passed, &failed, &checks);
#ifdef ST_HAVE_DRM_CACHE
	if (lkpi_x86_cpu_init() != 0) {
		(void) printf("FAIL drm_cache: no CLFLUSH on this processor\n");
		failed++;
	} else {
		st_run_suite("drm_cache", drm_cache_selftests,
		    drm_cache_nselftests, seed + 1000, scale, argc, argv, first,
		    &passed, &failed, &checks);
	}
#else
	(void) printf("drm_cache: not an x86-64 host, its tests not built\n");
#endif
	st_run_suite("drm_mm-host", drm_mm_host_tests, drm_mm_nhost_tests, seed,
	    scale, argc, argv, first, &passed, &failed, &checks);
	st_run_suite("rbtree", rbtree_tests, rbtree_ntests, seed, scale,
	    argc, argv, first, &passed, &failed, &checks);
	(void) printf("%u tests passed, %u failed, %llu checks\n", passed,
	    failed, (unsigned long long)checks);
	return (failed == 0 && passed > 0 ? 0 : 1);
}
