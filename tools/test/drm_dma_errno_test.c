/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The errno userland gets when the DMA binding of a new object fails, run
 * as root against rdrmnull attached with rdrmnull-dma-self.conf's node and
 * dma-max-transfer=4096 (tools/test/rdrmnull-dma-self-tiny.conf). A dumb
 * buffer of one page binds; one of four pages exceeds the largest transfer
 * and must fail with ENOMEM, as a failed object allocation does on Linux,
 * not with EFAULT. The failed object must not survive: the `gem` kstat of
 * rdrmnull (rdrmnull:0:gem) must show the same objects, handles, bytes of
 * object memory and DMA segments after the failure as before it.
 *
 * Build on illumos, with the patched libdrm headers in $INC:
 *   gcc -m64 -Wall -I$INC -o drm_dma_errno_test drm_dma_errno_test.c -lkstat
 * The exit status is the number of failed checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <kstat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioccom.h>

#include "drm.h"
#include "drm_mode.h"

static int failures;

static void
check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

/* What the gem kstat counts; a failed creation must change none of it. */
struct gem {
	uint64_t objects, handles, umem_bytes, dma_segments;
};

/* Reads the gem kstat of rdrmnull instance 0; returns 1 on success. */
static int
gem_stats(struct gem *g)
{
	static const char *const names[] = {
		"objects", "handles", "umem_bytes", "dma_segments"
	};
	uint64_t *out[] = {
		&g->objects, &g->handles, &g->umem_bytes, &g->dma_segments
	};
	kstat_ctl_t *kc;
	kstat_t *ksp;
	int i, ok = 0;

	(void) memset(g, 0xff, sizeof (*g));
	if ((kc = kstat_open()) == NULL)
		return (0);
	ksp = kstat_lookup(kc, "rdrmnull", 0, "gem");
	if (ksp != NULL && kstat_read(kc, ksp, NULL) != -1) {
		ok = 1;
		for (i = 0; i < 4; i++) {
			kstat_named_t *kn = kstat_data_lookup(ksp, (char *)names[i]);
			if (kn == NULL) {
				ok = 0;
				continue;
			}
			*out[i] = kn->value.ui64;
		}
	}
	(void) kstat_close(kc);
	printf("gem kstat: objects %llu handles %llu umem_bytes %llu "
	    "dma_segments %llu\n", (unsigned long long)g->objects,
	    (unsigned long long)g->handles, (unsigned long long)g->umem_bytes,
	    (unsigned long long)g->dma_segments);
	return (ok);
}

/* Creates a dumb buffer; returns 0 or the errno, and the handle in *h. */
static int
create_dumb(int fd, unsigned w, unsigned h32, unsigned *h)
{
	struct drm_mode_create_dumb c;

	memset(&c, 0, sizeof (c));
	c.width = w;
	c.height = h32;
	c.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0)
		return (errno);
	*h = c.handle;
	printf("created %ux%u: handle %u, size %llu\n", w, h32, c.handle,
	    (unsigned long long)c.size);
	return (0);
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
	unsigned h = 0;
	struct gem before, after;
	int fd, e, ok;

	if ((fd = open(path, O_RDWR)) < 0) {
		perror(path);
		return (1);
	}

	/* 32x32x32 bpp: exactly one page, within dma-max-transfer. */
	e = create_dumb(fd, 32, 32, &h);
	check(e == 0, "a one-page dumb buffer binds");
	if (e == 0) {
		struct drm_mode_destroy_dumb d = { .handle = h };
		check(ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) == 0,
		    "the one-page dumb buffer is destroyed");
	}

	/* 64x64x32 bpp: four pages, above dma-max-transfer. */
	ok = gem_stats(&before);
	h = 0;
	e = create_dumb(fd, 64, 64, &h);
	printf("four-page dumb buffer: errno %d (%s)\n", e, strerror(e));
	check(e == ENOMEM, "a four-page dumb buffer fails with ENOMEM");
	check(e != EFAULT, "the failed bind is not reported as EFAULT");
	ok = ok && gem_stats(&after);
	check(ok && memcmp(&before, &after, sizeof (before)) == 0,
	    "the failed object leaves no object, handle, memory or DMA segment");

	close(fd);
	printf("%d failed\n", failures);
	return (failures);
}
