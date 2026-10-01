/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The cross-driver test of illumos-drm R5a: since the DRM core is one
 * kernel module (misc/drm), a buffer, sync object or sync file of one
 * driver's device can be used by another driver's. Run as root while
 * cdrmnull (the C driver on the facade) and rdrmnull (the Rust driver on
 * the facade) are attached, and optionally virtio_gpu:
 *
 *	DRM_CARD_C=/dev/dri/cardN DRM_CARD_R=/dev/dri/cardM \
 *	    [DRM_CARD_V=/dev/dri/cardK] ./drm_r5a_test
 *
 * For each ordered pair of devices it exports a dumb buffer from the first
 * with PRIME_HANDLE_TO_FD, imports it into the second with
 * PRIME_FD_TO_HANDLE (and again: the same handle), maps it through both
 * devices and through the dma-buf, and checks that each mapping sees the
 * others' writes. cdrmnull's PRIME_SELFTEST ioctl drives the facade's
 * dma-buf functions (dma_buf_get, drm_gem_prime_import, drm_gem_prime_export,
 * dma_buf_fd) with the other driver's buffers. A fence of cdrmnull's test
 * timeline, as a sync file, is imported into a sync object of rdrmnull and
 * waited for there, and the other way round. At the end no GEM object is
 * left on either null device.
 *
 *	gcc -m64 -Wall -I drm -I<illumos-drm>/drivers/cdrmnull \
 *	    -o drm_r5a_test drm_r5a_test.c \
 *	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <kstat.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm.h"
#include "drm_mode.h"
#include "cdrmnull_ioctl.h"

/* rdrmnull's FENCE ioctl has the same layout and number as cdrmnull's. */
#define	RDRMNULL_IOCTL_FENCE	DRM_IOCTL_CDRMNULL_FENCE

static int failures, passes;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (ok)
		passes++;
	else
		failures++;
}

static int
xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return (r == -1 ? errno : 0);
}

struct dev {
	const char *name;	/* driver */
	const char *path;
	int fd;
};

static uint32_t
dumb(int fd, uint64_t *size, uint32_t *pitch)
{
	struct drm_mode_create_dumb c;

	(void) memset(&c, 0, sizeof (c));
	c.width = 128;
	c.height = 64;
	c.bpp = 32;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0)
		return (0);
	*size = c.size;
	*pitch = c.pitch;
	return (c.handle);
}

static uint32_t *
map_handle(int fd, uint32_t h, size_t size)
{
	struct drm_mode_map_dumb m;
	void *p;

	(void) memset(&m, 0, sizeof (m));
	m.handle = h;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) != 0)
		return (NULL);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	return (p == MAP_FAILED ? NULL : p);
}

static void
gem_close(int fd, uint32_t h)
{
	struct drm_gem_close c;

	(void) memset(&c, 0, sizeof (c));
	c.handle = h;
	(void) xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

static uint64_t
gem_objects(const char *drv)
{
	kstat_ctl_t *kc = kstat_open();
	kstat_t *ks;
	kstat_named_t *kn;
	uint64_t v = UINT64_MAX;

	if (kc == NULL)
		return (v);
	if ((ks = kstat_lookup(kc, (char *)drv, 0, "gem")) != NULL &&
	    kstat_read(kc, ks, NULL) != -1 &&
	    (kn = kstat_data_lookup(ks, "objects")) != NULL)
		v = kn->value.ui64;
	(void) kstat_close(kc);
	return (v);
}

/* A buffer of `a`, shared with `b` through a dma-buf. */
static void
share(struct dev *a, struct dev *b)
{
	char what[160];
	uint64_t size;
	uint32_t pitch, ha, hb, hb2, *pa, *pb, *pd;
	struct drm_prime_handle ph;
	int dfd, ok;
	size_t i, n;

	(void) printf("--- %s -> %s\n", a->name, b->name);
	ha = dumb(a->fd, &size, &pitch);
	(void) snprintf(what, sizeof (what), "%s: a 128x64 dumb buffer", a->name);
	check(ha != 0, what);
	if (ha == 0)
		return;
	n = (size_t)size / 4;
	pa = map_handle(a->fd, ha, size);
	(void) snprintf(what, sizeof (what), "%s: map it", a->name);
	check(pa != NULL, what);
	if (pa == NULL)
		return;
	for (i = 0; i < n; i++)
		pa[i] = 0xa5000000u | (uint32_t)i;

	(void) memset(&ph, 0, sizeof (ph));
	ph.handle = ha;
	ph.flags = DRM_CLOEXEC | DRM_RDWR;
	(void) snprintf(what, sizeof (what), "%s: export it (PRIME_HANDLE_TO_FD)", a->name);
	check(xioctl(a->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 && ph.fd >= 0, what);
	dfd = ph.fd;

	(void) memset(&ph, 0, sizeof (ph));
	ph.fd = dfd;
	(void) snprintf(what, sizeof (what), "%s: import %s's buffer (PRIME_FD_TO_HANDLE)",
	    b->name, a->name);
	check(xioctl(b->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == 0 && ph.handle != 0, what);
	hb = ph.handle;
	(void) memset(&ph, 0, sizeof (ph));
	ph.fd = dfd;
	(void) snprintf(what, sizeof (what), "%s: importing it again gives the same handle", b->name);
	check(xioctl(b->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == 0 && ph.handle == hb, what);
	hb2 = ph.handle;
	(void) hb2;

	pb = map_handle(b->fd, hb, size);
	(void) snprintf(what, sizeof (what), "%s: map its import", b->name);
	check(pb != NULL, what);
	if (pb != NULL) {
		for (ok = 1, i = 0; i < n && ok; i++)
			ok = pb[i] == (0xa5000000u | (uint32_t)i);
		(void) snprintf(what, sizeof (what), "%s sees what %s wrote", b->name, a->name);
		check(ok, what);
		for (i = 0; i < n; i++)
			pb[i] = 0x5a000000u | (uint32_t)i;
		for (ok = 1, i = 0; i < n && ok; i++)
			ok = pa[i] == (0x5a000000u | (uint32_t)i);
		(void) snprintf(what, sizeof (what), "%s sees what %s wrote", a->name, b->name);
		check(ok, what);
		(void) munmap((void *)pb, size);
	}
	pd = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
	(void) snprintf(what, sizeof (what), "the dma-buf of %s maps and shows the same memory", a->name);
	check(pd != MAP_FAILED && pd[1] == (0x5a000000u | 1u), what);
	if (pd != MAP_FAILED)
		(void) munmap((void *)pd, size);
	(void) munmap((void *)pa, size);
	(void) close(dfd);
	gem_close(b->fd, hb);
	gem_close(a->fd, ha);
}

/* cdrmnull's PRIME_SELFTEST: the facade's dma-buf functions. */
static void
selftest(struct dev *c, struct dev *o)
{
	char what[160];
	uint64_t size;
	uint32_t pitch, ho, *po, *pc;
	struct drm_prime_handle ph;
	struct cdrmnull_prime_selftest st;
	size_t i;
	int ok;

	(void) printf("--- PRIME_SELFTEST with %s\n", o->name);
	ho = dumb(o->fd, &size, &pitch);
	po = ho ? map_handle(o->fd, ho, size) : NULL;
	(void) snprintf(what, sizeof (what), "%s: a mapped dumb buffer", o->name);
	check(po != NULL, what);
	if (po == NULL)
		return;
	for (i = 0; i < size / 4; i++)
		po[i] = 0xc0de0000u | (uint32_t)i;
	(void) memset(&ph, 0, sizeof (ph));
	ph.handle = ho;
	ph.flags = DRM_CLOEXEC | DRM_RDWR;
	check(xioctl(o->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0, "export it");
	(void) memset(&st, 0, sizeof (st));
	st.fd = ph.fd;
	st.flags = CDRMNULL_PRIME_IMPORT;
	(void) snprintf(what, sizeof (what),
	    "cdrmnull imports %s's dma-buf (dma_buf_get, drm_gem_prime_import)", o->name);
	check(xioctl(c->fd, DRM_IOCTL_CDRMNULL_PRIME_SELFTEST, &st) == 0 && st.handle != 0, what);
	pc = st.handle ? map_handle(c->fd, st.handle, size) : NULL;
	for (ok = pc != NULL, i = 0; ok && i < size / 4; i++)
		ok = pc[i] == (0xc0de0000u | (uint32_t)i);
	check(ok, "cdrmnull maps the import and sees the exporter's data");
	if (pc != NULL)
		(void) munmap((void *)pc, size);
	(void) close(ph.fd);

	/* And back: cdrmnull exports its import with dma_buf_fd. */
	st.fd = -1;
	st.flags = CDRMNULL_PRIME_EXPORT;
	check(xioctl(c->fd, DRM_IOCTL_CDRMNULL_PRIME_SELFTEST, &st) == 0 && st.fd >= 0,
	    "cdrmnull exports it again (drm_gem_prime_export, dma_buf_fd)");
	if (st.fd >= 0) {
		uint32_t *pd = mmap(NULL, size, PROT_READ, MAP_SHARED, st.fd, 0);

		check(pd != MAP_FAILED && pd[3] == (0xc0de0000u | 3u),
		    "the re-exported dma-buf maps the original memory");
		if (pd != MAP_FAILED)
			(void) munmap((void *)pd, size);
		(void) memset(&ph, 0, sizeof (ph));
		ph.fd = st.fd;
		(void) snprintf(what, sizeof (what), "%s imports cdrmnull's re-export", o->name);
		check(xioctl(o->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == 0 && ph.handle != 0, what);
		if (ph.handle != 0 && ph.handle != ho)
			gem_close(o->fd, ph.handle);
		(void) close(st.fd);
	}
	gem_close(c->fd, st.handle);
	(void) munmap((void *)po, size);
	gem_close(o->fd, ho);
}

/* A test-timeline fence of `a` as a sync file, waited for through `b`. */
static void
fences(struct dev *a, struct dev *b)
{
	char what[160];
	struct cdrmnull_fence nf;
	uint32_t so = 0, first;
	int r;

	(void) printf("--- a fence of %s waited for by %s\n", a->name, b->name);
	(void) memset(&nf, 0, sizeof (nf));
	nf.delay_ms = 200;
	nf.flags = CDRMNULL_FENCE_SYNC_FILE;
	nf.fd = -1;
	(void) snprintf(what, sizeof (what), "%s: a 200 ms test fence as a sync file", a->name);
	check(xioctl(a->fd, RDRMNULL_IOCTL_FENCE, &nf) == 0 && nf.fd >= 0, what);
	if (nf.fd < 0)
		return;
	r = drmSyncobjCreate(b->fd, 0, &so);
	(void) snprintf(what, sizeof (what), "%s imports it into a sync object", b->name);
	check(r == 0 && drmSyncobjImportSyncFile(b->fd, so, nf.fd) == 0, what);
	r = drmSyncobjWait(b->fd, &so, 1, INT64_MAX, 0, &first);
	(void) snprintf(what, sizeof (what), "%s waits for %s's fence", b->name, a->name);
	check(r == 0, what);
	(void) drmSyncobjDestroy(b->fd, so);
	(void) close(nf.fd);
}

int
main(void)
{
	struct dev d[3];
	int nd = 0, i, j, tries;
	const char *s;

	(void) memset(d, 0, sizeof (d));
	if ((s = getenv("DRM_CARD_C")) == NULL || *s == '\0' ||
	    getenv("DRM_CARD_R") == NULL) {
		(void) fprintf(stderr, "DRM_CARD_C and DRM_CARD_R must name the cards\n");
		return (2);
	}
	d[nd].path = s;
	d[nd].name = "cdrmnull";
	nd++;
	d[nd].path = getenv("DRM_CARD_R");
	d[nd].name = "rdrmnull";
	nd++;
	if ((s = getenv("DRM_CARD_V")) != NULL && *s != '\0') {
		d[nd].path = s;
		d[nd].name = "virtio_gpu";
		nd++;
	}
	for (i = 0; i < nd; i++) {
		d[i].fd = open(d[i].path, O_RDWR);
		(void) printf("INFO %s is %s\n", d[i].name, d[i].path);
		check(d[i].fd >= 0, "open the card node");
		if (d[i].fd < 0)
			return (1);
	}
	for (i = 0; i < nd; i++) {
		for (j = 0; j < nd; j++) {
			if (i != j)
				share(&d[i], &d[j]);
		}
	}
	for (i = 1; i < nd; i++)
		selftest(&d[0], &d[i]);
	fences(&d[0], &d[1]);
	fences(&d[1], &d[0]);
	for (i = 0; i < nd; i++)
		(void) close(d[i].fd);
	/* Releases from interrupt context finish on a task queue. */
	for (tries = 0; tries < 40; tries++) {
		if (gem_objects("cdrmnull") == 0 && gem_objects("rdrmnull") == 0)
			break;
		(void) usleep(50000);
	}
	check(gem_objects("cdrmnull") == 0 && gem_objects("rdrmnull") == 0,
	    "no GEM object is left on either null device");
	(void) printf("drm_r5a_test: %d passed, %d failed\n", passes, failures);
	return (failures != 0);
}
