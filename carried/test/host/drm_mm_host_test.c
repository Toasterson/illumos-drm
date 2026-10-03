/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host-only tests of the carried drm_mm: the functions misc/drm_helpers
 * compiles but does not export (drm_mm_print(), which i915 does not
 * import) and the warning of drm_mm_takedown() on an allocator that still
 * holds nodes (counted by the host's linux/bug.h).
 *
 * Origin: "print" re-expresses the "debug" case of Linux's KUnit suite
 * drivers/gpu/drm/tests/drm_mm_test.c (GPL-2.0-only), which prints an
 * allocator with two nodes; it is not a copy.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <linux/kernel.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>

#include "selftest.h"

extern unsigned long lkpi_host_warnings;

struct st_buf {
	char text[1024];
	size_t len;
};

static void
st_buf_printfn(struct drm_printer *p, struct va_format *vaf)
{
	struct st_buf *b = p->arg;
	int n;

	if (b->len >= sizeof (b->text))
		return;
	n = vsnprintf(b->text + b->len, sizeof (b->text) - b->len, vaf->fmt,
	    *vaf->va);
	if (n > 0)
		b->len += (size_t)n;
}

static unsigned int
st_count(const char *text, const char *word)
{
	unsigned int n = 0;

	while ((text = strstr(text, word)) != NULL) {
		n++;
		text += strlen(word);
	}
	return (n);
}

static void
st_mm_print(struct st_ctx *ctx)
{
	struct drm_mm mm;
	struct drm_mm_node a, b;
	struct st_buf buf;
	struct drm_printer p;

	memset(&mm, 0, sizeof (mm));
	memset(&a, 0, sizeof (a));
	memset(&b, 0, sizeof (b));
	memset(&buf, 0, sizeof (buf));
	memset(&p, 0, sizeof (p));
	p.printfn = st_buf_printfn;
	p.arg = &buf;

	drm_mm_init(&mm, 0, 4096);
	a.start = 0;
	a.size = 512;
	b.start = 1024;
	b.size = 512;
	ST_CHECK(ctx, drm_mm_reserve_node(&mm, &a) == 0, "reserve a");
	ST_CHECK(ctx, drm_mm_reserve_node(&mm, &b) == 0, "reserve b");
	drm_mm_print(&mm, &p);
	ST_CHECK(ctx, strstr(buf.text, "total: 4096, used 1024 free 3072") !=
	    NULL, "summary line missing in:\n%s", buf.text);
	ST_CHECK(ctx, st_count(buf.text, ": used\n") == 2 &&
	    st_count(buf.text, ": free\n") == 2,
	    "expected two used and two free ranges in:\n%s", buf.text);
	ST_CHECK(ctx, strstr(buf.text, "0x0000000000000600-0x0000000000001000: "
	    "2560: free") != NULL, "last hole missing in:\n%s", buf.text);
	drm_mm_remove_node(&a);
	drm_mm_remove_node(&b);
	drm_mm_takedown(&mm);
}

static void
st_mm_takedown_dirty(struct st_ctx *ctx)
{
	struct drm_mm mm;
	struct drm_mm_node a;
	unsigned long before;

	memset(&mm, 0, sizeof (mm));
	memset(&a, 0, sizeof (a));
	drm_mm_init(&mm, 0, 100);
	a.start = 10;
	a.size = 10;
	ST_CHECK(ctx, drm_mm_reserve_node(&mm, &a) == 0, "reserve");
	before = lkpi_host_warnings;
	(void) fprintf(stderr, "(the next warning is expected)\n");
	drm_mm_takedown(&mm);
	ST_CHECK(ctx, lkpi_host_warnings == before + 1,
	    "takedown of a used allocator did not warn");
	drm_mm_remove_node(&a);
	before = lkpi_host_warnings;
	drm_mm_takedown(&mm);
	ST_CHECK(ctx, lkpi_host_warnings == before,
	    "takedown of a clean allocator warned");
}

const struct st_test drm_mm_host_tests[] = {
	{ "print", st_mm_print, ST_HOST_ONLY },
	{ "takedown_dirty", st_mm_takedown_dirty, ST_HOST_ONLY },
};
const unsigned int drm_mm_nhost_tests = ARRAY_SIZE(drm_mm_host_tests);
