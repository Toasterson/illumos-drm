/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The host tests' minimal harness: CHECK() counts checks and failures,
 * TEST() defines a test case that main.c runs (see tests.def).
 */
#ifndef _LKPI_HOST_TEST_H
#define	_LKPI_HOST_TEST_H

#include <stdio.h>
#include <string.h>

extern unsigned long t_checks, t_failures;
extern const char *t_current;

#define	CHECK(c)	do { \
	t_checks++; \
	if (!(c)) { \
		t_failures++; \
		fprintf(stderr, "%s:%d: [%s] CHECK failed: %s\n", __FILE__, \
		    __LINE__, t_current, #c); \
	} \
	} while (0)

#define	CHECK_EQ(a, b)	do { \
	long long __a = (long long)(a), __b = (long long)(b); \
	t_checks++; \
	if (__a != __b) { \
		t_failures++; \
		fprintf(stderr, "%s:%d: [%s] CHECK_EQ failed: %s == %lld, " \
		    "expected %s == %lld\n", __FILE__, __LINE__, t_current, \
		    #a, __a, #b, __b); \
	} \
	} while (0)

#define	CHECK_STR(a, b)	do { \
	const char *__a = (a), *__b = (b); \
	t_checks++; \
	if (__a == NULL || strcmp(__a, __b) != 0) { \
		t_failures++; \
		fprintf(stderr, "%s:%d: [%s] CHECK_STR failed: \"%s\", " \
		    "expected \"%s\"\n", __FILE__, __LINE__, t_current, \
		    __a != NULL ? __a : "(NULL)", __b); \
	} \
	} while (0)

#define	TEST(name)	void test_##name(void)

/* A deterministic pseudo-random sequence for the randomised tests. */
static inline unsigned long
t_rand(unsigned long long *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return ((unsigned long)*s);
}

/* From host_stubs.c. */
extern int lkpi_host_cmn_level;
extern char lkpi_host_cmn_msg[1024];
extern unsigned long lkpi_host_cmn_count;
extern int lkpi_host_cmn_quiet;
extern int lkpi_host_kmem_fail_nosleep;
extern unsigned long lkpi_host_kmem_live;
extern int lkpi_host_random_fail;

#endif
