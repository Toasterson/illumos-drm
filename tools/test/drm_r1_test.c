/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Userland test of the illumos-drm R1 core (GEM objects, handles, names,
 * mmap through devmap(9E), dumb buffers), run as root against rdrmnull.
 *
 * Dumb buffers are created, mapped and destroyed with raw ioctls through
 * drmIoctl() from the installed libdrm 2.4.109; authentication uses
 * drmGetMagic/drmAuthMagic. The `gem` kstat of rdrmnull
 * (rdrmnull:0:gem) is read with libkstat to check that every object,
 * handle, name, mapping and byte of object memory is accounted for and
 * released. The driver-private ioctls of rdrmnull drive the refault
 * experiment (an object whose backing moves while it is mapped) and the
 * release of a reference from interrupt context.
 *
 * Every check prints PASS or FAIL; the exit status is the number of
 * failures (capped at 255 by the shell).
 *
 * Build on illumos, with the patched libdrm headers in $INC:
 *   gcc -m64 -Wall -I$INC -o drm_r1_test drm_r1_test.c \
 *       -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread
 * and the 32-bit probe (no libdrm):
 *   gcc -m32 -Wall -DPROBE32 -I$INC -o drm_r1_probe32 drm_r1_test.c
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "drm.h"
#include "drm_mode.h"

#define	CARD	"/dev/dri/card0"
#define	RENDER	"/dev/dri/renderD128"
#define	NOBODY	60001
#define	PAGE	4096
/* DRM_FILE_PAGE_OFFSET_START on 64-bit Linux, and so here. */
#define	GEM_OFFSET_START	0x10000000ULL

/* The driver-private ioctls of rdrmnull (examples/rdrmnull/src/lib.rs). */
struct rdrmnull_create_movable {
	uint64_t size;
	uint32_t handle;
	uint32_t pad;
};
struct rdrmnull_move {
	uint32_t handle;
	uint32_t flags;
	uint32_t pieces;
	uint32_t backing;
};
struct rdrmnull_put_later {
	uint32_t handle;
	uint32_t delay_ms;
};
_Static_assert(sizeof (struct rdrmnull_create_movable) == 16, "layout");
_Static_assert(sizeof (struct rdrmnull_move) == 16, "layout");
_Static_assert(sizeof (struct rdrmnull_put_later) == 8, "layout");
#define	RDRMNULL_IOCTL_CREATE_MOVABLE \
	DRM_IOWR(DRM_COMMAND_BASE + 0x00, struct rdrmnull_create_movable)
#define	RDRMNULL_IOCTL_MOVE DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct rdrmnull_move)
#define	RDRMNULL_IOCTL_PUT_LATER DRM_IOW(DRM_COMMAND_BASE + 0x02, struct rdrmnull_put_later)
#define	MOVE_FILL		(1u << 8)
#define	MOVE_UNLOAD_ONLY	(1u << 9)

static int failures;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
	(void) fflush(stdout);
}

#ifdef PROBE32

/* 32-bit processes get ENOTSUP for the GEM, dumb and driver ioctls. */
int
main(void)
{
	struct drm_gem_close gc;
	struct drm_mode_create_dumb cd;
	struct rdrmnull_move mv;
	int fd = open(CARD, O_RDWR);

	check(fd >= 0, "32-bit open " CARD);
	if (fd < 0)
		return (1);
	(void) memset(&gc, 0, sizeof (gc));
	errno = 0;
	check(ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == -1 && errno == ENOTSUP,
	    "32-bit GEM_CLOSE fails with ENOTSUP");
	(void) memset(&cd, 0, sizeof (cd));
	cd.width = cd.height = 16;
	cd.bpp = 32;
	errno = 0;
	check(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == -1 && errno == ENOTSUP,
	    "32-bit MODE_CREATE_DUMB fails with ENOTSUP");
	(void) memset(&mv, 0, sizeof (mv));
	errno = 0;
	check(ioctl(fd, RDRMNULL_IOCTL_MOVE, &mv) == -1 && errno == ENOTSUP,
	    "32-bit driver ioctl fails with ENOTSUP");
	(void) close(fd);
	(void) printf("probe32: %d failures\n", failures);
	return (failures);
}

#else	/* PROBE32 */

#include <kstat.h>
#include <xf86drm.h>

/* --- the gem kstat --- */

struct gem {
	uint64_t objects, umem_bytes, dma_segments, handles, names, offsets;
	uint64_t mappings, pending_maps, deferred_frees, drain_tasks, faults;
};

static int
gem_stats(struct gem *g)
{
	static const char *const names[] = {
		"objects", "umem_bytes", "dma_segments", "handles", "names",
		"offsets", "mappings", "pending_maps", "deferred_frees",
		"drain_tasks", "faults"
	};
	uint64_t *out[] = {
		&g->objects, &g->umem_bytes, &g->dma_segments, &g->handles,
		&g->names, &g->offsets, &g->mappings, &g->pending_maps,
		&g->deferred_frees, &g->drain_tasks, &g->faults
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
		for (i = 0; i < (int)(sizeof (names) / sizeof (names[0])); i++) {
			kstat_named_t *kn = kstat_data_lookup(ksp, (char *)names[i]);
			if (kn == NULL) {
				ok = 0;
				continue;
			}
			*out[i] = kn->value.ui64;
		}
	}
	(void) kstat_close(kc);
	return (ok);
}

static void
print_gem(const char *when, const struct gem *g)
{
	(void) printf("INFO gem kstat %s: objects %llu umem_bytes %llu dma_segments %llu "
	    "handles %llu names %llu offsets %llu mappings %llu pending_maps %llu "
	    "deferred_frees %llu drain_tasks %llu faults %llu\n", when,
	    (unsigned long long)g->objects, (unsigned long long)g->umem_bytes,
	    (unsigned long long)g->dma_segments, (unsigned long long)g->handles,
	    (unsigned long long)g->names, (unsigned long long)g->offsets,
	    (unsigned long long)g->mappings, (unsigned long long)g->pending_maps,
	    (unsigned long long)g->deferred_frees, (unsigned long long)g->drain_tasks,
	    (unsigned long long)g->faults);
}

/* Every gauge is zero: nothing is allocated, handled, named or mapped. */
static int
gem_idle(const char *when)
{
	struct gem g;
	int ok = gem_stats(&g);

	print_gem(when, &g);
	return (ok && g.objects == 0 && g.umem_bytes == 0 && g.dma_segments == 0 &&
	    g.handles == 0 && g.names == 0 && g.offsets == 0 && g.mappings == 0 &&
	    g.pending_maps == 0);
}

/* --- ioctl helpers (return 0 or errno) --- */

static int
xioctl(int fd, unsigned long req, void *arg)
{
	return (drmIoctl(fd, req, arg) == 0 ? 0 : errno);
}

static int
create_dumb(int fd, uint32_t w, uint32_t h, uint32_t bpp, uint32_t flags,
    uint32_t *handle, uint32_t *pitch, uint64_t *size)
{
	struct drm_mode_create_dumb c;
	int e;

	(void) memset(&c, 0, sizeof (c));
	c.width = w;
	c.height = h;
	c.bpp = bpp;
	c.flags = flags;
	e = xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c);
	if (e == 0) {
		if (handle != NULL)
			*handle = c.handle;
		if (pitch != NULL)
			*pitch = c.pitch;
		if (size != NULL)
			*size = c.size;
	}
	return (e);
}

static int
map_dumb(int fd, uint32_t handle, uint32_t pad, uint64_t *off)
{
	struct drm_mode_map_dumb m;
	int e;

	(void) memset(&m, 0, sizeof (m));
	m.handle = handle;
	m.pad = pad;
	e = xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m);
	if (e == 0 && off != NULL)
		*off = m.offset;
	return (e);
}

static int
destroy_dumb(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb d;

	d.handle = handle;
	return (xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d));
}

static int
gem_close(int fd, uint32_t handle, uint32_t pad)
{
	struct drm_gem_close c;

	c.handle = handle;
	c.pad = pad;
	return (xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c));
}

static int
gem_flink(int fd, uint32_t handle, uint32_t *name)
{
	struct drm_gem_flink f;
	int e;

	(void) memset(&f, 0, sizeof (f));
	f.handle = handle;
	e = xioctl(fd, DRM_IOCTL_GEM_FLINK, &f);
	if (e == 0)
		*name = f.name;
	return (e);
}

static int
gem_open(int fd, uint32_t name, uint32_t *handle, uint64_t *size)
{
	struct drm_gem_open o;
	int e;

	(void) memset(&o, 0, sizeof (o));
	o.name = name;
	e = xioctl(fd, DRM_IOCTL_GEM_OPEN, &o);
	if (e == 0) {
		*handle = o.handle;
		if (size != NULL)
			*size = o.size;
	}
	return (e);
}

/* mmap(2), returning the mapping or NULL with errno set. */
static uint32_t *
map(int fd, size_t len, int flags, uint64_t off)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, flags, fd, (off_t)off);

	return (p == MAP_FAILED ? NULL : p);
}

static uint32_t
pattern(size_t i, uint32_t seed)
{
	return ((uint32_t)(i * 2654435761u) ^ seed);
}

static void
fill(uint32_t *p, size_t bytes, uint32_t seed)
{
	size_t i;

	for (i = 0; i < bytes / 4; i++)
		p[i] = pattern(i, seed);
}

/* Returns the index of the first mismatch, or (size_t)-1. */
static size_t
verify(const uint32_t *p, size_t bytes, uint32_t seed)
{
	size_t i;

	for (i = 0; i < bytes / 4; i++)
		if (p[i] != pattern(i, seed))
			return (i);
	return ((size_t)-1);
}

static int
all_bytes(const void *p, size_t bytes, uint8_t v)
{
	const uint8_t *b = p;
	size_t i;

	for (i = 0; i < bytes; i++)
		if (b[i] != v)
			return (0);
	return (1);
}

static void
drop_to_nobody(void)
{
	if (setgid(NOBODY) != 0 || setuid(NOBODY) != 0) {
		perror("setuid nobody");
		exit(100);
	}
}

static void
msleep(int ms)
{
	(void) usleep(ms * 1000);
}

/* --- capabilities --- */

static void
test_caps(int fd, const char *node)
{
	char what[128];
	uint64_t v;

	v = 0xdead;
	(void) snprintf(what, sizeof (what), "%s: DRM_CAP_DUMB_BUFFER is 1", node);
	check(drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &v) == 0 && v == 1, what);
	v = 0xdead;
	(void) snprintf(what, sizeof (what), "%s: DRM_CAP_DUMB_PREFERRED_DEPTH is 24", node);
	check(drmGetCap(fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &v) == 0 && v == 24, what);
	v = 0xdead;
	(void) snprintf(what, sizeof (what), "%s: DRM_CAP_DUMB_PREFER_SHADOW is 1", node);
	check(drmGetCap(fd, DRM_CAP_DUMB_PREFER_SHADOW, &v) == 0 && v == 1, what);
	v = 0xdead;
	(void) snprintf(what, sizeof (what), "%s: DRM_CAP_ADDFB2_MODIFIERS is 1 since R3", node);
	check(drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &v) == 0 && v == 1, what);
}

/* --- dumb buffers: arguments --- */

static void
test_dumb_args(int fd)
{
	uint32_t h, pitch;
	uint64_t size, off;

	check(create_dumb(fd, 0, 16, 32, 0, &h, NULL, NULL) == EINVAL, "CREATE_DUMB width 0 is EINVAL");
	check(create_dumb(fd, 16, 0, 32, 0, &h, NULL, NULL) == EINVAL, "CREATE_DUMB height 0 is EINVAL");
	check(create_dumb(fd, 16, 16, 0, 0, &h, NULL, NULL) == EINVAL, "CREATE_DUMB bpp 0 is EINVAL");
	check(create_dumb(fd, 16, 16, 0xfffffff8u, 0, &h, NULL, NULL) == EINVAL, "CREATE_DUMB bpp above U32_MAX - 8 is EINVAL");
	check(create_dumb(fd, 100, 1, 12, 0, &h, &pitch, &size) == 0 && pitch == 256 && destroy_dumb(fd, h) == 0,
	    "CREATE_DUMB bpp 12 is accepted as 2 bytes per pixel, as in Linux (pitch 256)");
	check(create_dumb(fd, 16, 16, 32, 1, &h, &pitch, NULL) == 0 && pitch == 64 && destroy_dumb(fd, h) == 0,
	    "CREATE_DUMB flags are ignored, as in Linux");
	check(create_dumb(fd, 0x40000000, 1, 32, 0, &h, NULL, NULL) == EINVAL,
	    "CREATE_DUMB width*4 overflowing 32 bits is EINVAL");
	check(create_dumb(fd, 65536, 65536, 32, 0, &h, NULL, NULL) == EINVAL,
	    "CREATE_DUMB pitch*height overflowing 32 bits is EINVAL");
	check(create_dumb(fd, 16384, 8192, 32, 0, &h, NULL, NULL) == ENOMEM,
	    "CREATE_DUMB of 512 MiB (above rdrmnull's 16 MiB object limit) is ENOMEM");
	check(create_dumb(fd, 2048, 2049, 32, 0, &h, NULL, NULL) == ENOMEM,
	    "CREATE_DUMB of one row more than 16 MiB is ENOMEM");
	check(create_dumb(fd, 100, 10, 24, 0, &h, &pitch, &size) == 0 && pitch == 320 && size == PAGE,
	    "CREATE_DUMB 100x10x24: pitch 320 (64-byte rows), size one page");
	check(map_dumb(fd, h, 1, &off) == 0, "MAP_DUMB ignores pad, as in Linux");
	check(map_dumb(fd, h + 1000, 0, &off) == ENOENT, "MAP_DUMB of an unknown handle is ENOENT");
	check(map_dumb(fd, 0, 0, &off) == ENOENT, "MAP_DUMB of handle 0 is ENOENT");
	check(destroy_dumb(fd, h) == 0, "DESTROY_DUMB");
	check(destroy_dumb(fd, h) == EINVAL, "DESTROY_DUMB of a destroyed handle is EINVAL");
	check(gem_idle("after the argument checks"), "nothing is left after the argument checks");
}

/* rdrmnull limits all objects together to 64 MiB: four 16 MiB buffers. */
static void
test_memory_limit(int fd)
{
	uint32_t h[5];
	uint64_t size;
	struct gem g;
	int i, n = 0, e = 0;

	for (i = 0; i < 5; i++) {
		e = create_dumb(fd, 2048, 2048, 32, 0, &h[i], NULL, &size);
		if (e != 0)
			break;
		n++;
	}
	(void) gem_stats(&g);
	print_gem("at the memory limit", &g);
	check(n == 4 && e == ENOMEM && g.umem_bytes == 64 << 20,
	    "four 16 MiB dumb buffers fit the 64 MiB device limit, the fifth is ENOMEM");
	for (i = 0; i < n; i++)
		(void) destroy_dumb(fd, h[i]);
	check(create_dumb(fd, 2048, 2048, 32, 0, &h[0], NULL, NULL) == 0 && destroy_dumb(fd, h[0]) == 0,
	    "memory is available again after destroying them");
	check(gem_idle("after the memory limit"), "nothing is left after the memory limit test");
}

/* --- the main dumb-buffer scenario --- */

/*
 * The child side of the FLINK/OPEN checks: a separate process that opens
 * its own file (nothing inherited is used), as root or as nobody.
 */
static int
open_by_name(uint32_t name, uint64_t off, uint64_t size, uint32_t seed, int as_nobody,
    int to_parent, int from_parent)
{
	uint32_t h, *p;
	uint64_t sz, off2;
	drm_magic_t magic;
	int fd, rfd;
	char ok;

	if (as_nobody)
		drop_to_nobody();
	fd = open(CARD, O_RDWR);
	check(fd >= 0, as_nobody ? "nobody: open card0" : "separate process: open card0");
	if (fd < 0)
		return (failures);
	/* A file without a handle to the object must not map it. */
	errno = 0;
	p = map(fd, size, MAP_SHARED, off);
	check(p == NULL && errno == ENXIO, "mmap of an object without a handle in this file fails (ENXIO)");
	if (as_nobody) {
		check(gem_open(fd, name, &h, &sz) == EACCES, "nobody: GEM_OPEN before authentication is EACCES");
		check(drmGetMagic(fd, &magic) == 0, "nobody: drmGetMagic");
		(void) write(to_parent, &magic, sizeof (magic));
		(void) read(from_parent, &ok, 1);
		rfd = open(RENDER, O_RDWR);
		check(rfd >= 0, "nobody: open renderD128");
		check(gem_open(rfd, name, &h, &sz) == EACCES, "nobody: GEM_OPEN on the render node is EACCES");
		check(gem_flink(rfd, 1, &h) == EACCES, "nobody: GEM_FLINK on the render node is EACCES");
		check(create_dumb(rfd, 16, 16, 32, 0, &h, NULL, NULL) == EACCES,
		    "nobody: CREATE_DUMB on the render node is EACCES");
		check(gem_close(rfd, 7, 0) == EINVAL, "nobody: GEM_CLOSE on the render node is allowed (EINVAL for handle 7)");
		(void) close(rfd);
	}
	check(gem_open(fd, 0, &h, &sz) == ENOENT, "GEM_OPEN of name 0 is ENOENT");
	check(gem_open(fd, name + 1000, &h, &sz) == ENOENT, "GEM_OPEN of an unknown name is ENOENT");
	check(gem_open(fd, name, &h, &sz) == 0 && sz == size,
	    as_nobody ? "nobody: GEM_OPEN after authentication, size matches" :
	    "separate process: GEM_OPEN by name, size matches");
	check(h == 1, "the first handle of a file is 1");
	check(map_dumb(fd, h, 0, &off2) == 0 && off2 == off, "MAP_DUMB of the opened handle gives the same offset");
	p = map(fd, size, MAP_SHARED, off);
	check(p != NULL, "mmap through the opened handle");
	if (p != NULL) {
		check(verify(p, size, seed) == (size_t)-1, "the other process reads the pattern");
		p[1] = 0xfeedf00d;
		(void) munmap((void *)p, size);
	}
	check(gem_close(fd, h, 0) == 0, "GEM_CLOSE of the opened handle");
	(void) close(fd);
	return (failures);
}

static void
test_dumb(int fd)
{
	uint32_t h, h2, pitch, name, name2, *p, *q, *r;
	uint64_t size, off, off2, sz;
	struct gem g;
	pid_t pid;
	int status, e, pc[2], cp[2];
	drm_magic_t magic;
	char ok = 1;
	size_t bad;

	check(create_dumb(fd, 640, 480, 32, 0, &h, &pitch, &size) == 0, "CREATE_DUMB 640x480x32");
	(void) printf("INFO dumb handle %u pitch %u size %llu\n", h, pitch, (unsigned long long)size);
	check(h == 1, "the first handle of a file is 1");
	check(pitch == 2560 && size == 2560 * 480, "pitch 2560 and size 1228800");
	check(gem_stats(&g) && g.objects == 1 && g.handles == 1 && g.umem_bytes == size && g.offsets == 0,
	    "kstat: one object, one handle, 1228800 bytes, no offset yet");
	print_gem("after CREATE_DUMB", &g);
	/*
	 * rdrmnull is a pseudo device: no DMA binding, no scatter/gather
	 * table. Under the DMA drill (RDRMNULL_DMA=1, set by run-r1-test.sh
	 * for a .conf with dma-mode "self" or "root-node") every object is
	 * bound, with at most one cookie per page.
	 */
	if (getenv("RDRMNULL_DMA") != NULL && strcmp(getenv("RDRMNULL_DMA"), "1") == 0) {
		check(g.dma_segments >= 1 && g.dma_segments <= size / PAGE,
		    "kstat: the object's memory is bound for DMA (scatter/gather entries)");
	} else {
		check(g.dma_segments == 0, "kstat: no DMA segments (rdrmnull does no DMA)");
	}

	check(map_dumb(fd, h, 0, &off) == 0, "MAP_DUMB");
	(void) printf("INFO fake offset 0x%llx\n", (unsigned long long)off);
	check(off >= GEM_OFFSET_START && off % PAGE == 0, "the offset is page-aligned and above 256 MiB");
	check(map_dumb(fd, h, 0, &off2) == 0 && off2 == off, "MAP_DUMB again gives the same offset");
	check(gem_stats(&g) && g.offsets == 1, "kstat: one offset");

	p = map(fd, size, MAP_SHARED, off);
	check(p != NULL, "mmap the dumb buffer");
	if (p == NULL) {
		(void) printf("INFO mmap errno %d\n", errno);
		return;
	}
	check(all_bytes(p, size, 0), "a new dumb buffer reads as zeros");
	fill(p, size, 0x5eed);
	check(gem_stats(&g) && g.mappings == 1, "kstat: one mapping");

	/* Refused mappings; devmap_setup(9F) reports all of them as ENXIO. */
	errno = 0;
	check(map(fd, size, MAP_SHARED, off + PAGE) == NULL && errno == ENXIO,
	    "mmap at an offset inside the object is ENXIO");
	(void) gem_stats(&g);
	print_gem("after mmap inside the object", &g);
	errno = 0;
	check(map(fd, size + PAGE, MAP_SHARED, off) == NULL && errno == ENXIO,
	    "mmap beyond the end of the object is ENXIO");
	(void) gem_stats(&g);
	print_gem("after mmap beyond the end", &g);
	errno = 0;
	check(map(fd, PAGE, MAP_PRIVATE, off) == NULL && errno == ENXIO, "MAP_PRIVATE is refused (ENXIO)");
	(void) printf("INFO MAP_PRIVATE errno %d\n", errno);
	(void) gem_stats(&g);
	print_gem("after MAP_PRIVATE", &g);
	errno = 0;
	check(map(fd, PAGE, MAP_SHARED, GEM_OFFSET_START - PAGE) == NULL && errno == ENXIO,
	    "mmap below the GEM offset space is ENXIO");
	(void) gem_stats(&g);
	print_gem("after the refused mappings", &g);
	check(g.pending_maps == 0 && g.mappings == 1, "kstat: refused mappings leave nothing behind");

	/* A second, short mapping in the same process sees the same memory. */
	q = map(fd, 100, MAP_SHARED, off);
	check(q != NULL, "mmap of 100 bytes (rounded to a page)");
	if (q != NULL) {
		check(verify(q, 100, 0x5eed) == (size_t)-1, "the second mapping shows the pattern");
		(void) munmap((void *)q, 100);
	}

	/* Fork: the child reads through the inherited mapping and a new one. */
	pid = fork();
	if (pid == 0) {
		failures = 0;
		check(verify(p, size, 0x5eed) == (size_t)-1, "child: inherited mapping shows the pattern");
		q = map(fd, size, MAP_SHARED, off);
		check(q != NULL, "child: mmap through the inherited file");
		if (q != NULL) {
			check(verify(q, size, 0x5eed) == (size_t)-1, "child: its own mapping shows the pattern");
			q[size / 4 - 1] = 0xc0ffee;
			(void) munmap((void *)q, size);
		}
		_exit(failures);
	}
	(void) waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "forked child checks (see above)");
	check(p[size / 4 - 1] == 0xc0ffee, "the parent sees the child's write");
	p[size / 4 - 1] = pattern(size / 4 - 1, 0x5eed);
	check(gem_stats(&g) && g.mappings == 1, "kstat: the child's mappings are gone with the child");

	/* Partial munmap splits the mapping. */
	r = map(fd, 3 * PAGE, MAP_SHARED, off);
	check(r != NULL, "mmap three pages");
	if (r != NULL) {
		check(munmap((char *)r + PAGE, PAGE) == 0, "munmap the middle page");
		check(gem_stats(&g) && g.mappings == 3, "kstat: the split leaves two recorded pieces (3 mappings)");
		check(r[0] == pattern(0, 0x5eed) && r[2 * PAGE / 4] == pattern(2 * PAGE / 4, 0x5eed),
		    "both remaining pieces still show the pattern");
		(void) munmap((void *)r, PAGE);
		(void) munmap((char *)r + 2 * PAGE, PAGE);
		check(gem_stats(&g) && g.mappings == 1, "kstat: unmapping both pieces leaves one mapping");
	}

	/* FLINK and OPEN. */
	check(gem_flink(fd, h, &name) == 0 && name != 0, "GEM_FLINK");
	(void) printf("INFO flink name %u\n", name);
	check(gem_flink(fd, h, &name2) == 0 && name2 == name, "GEM_FLINK again returns the same name");
	check(gem_flink(fd, h + 1000, &name2) == ENOENT, "GEM_FLINK of an unknown handle is ENOENT");
	check(gem_stats(&g) && g.names == 1, "kstat: one name");

	pid = fork();
	if (pid == 0) {
		failures = 0;
		_exit(open_by_name(name, off, size, 0x5eed, 0, -1, -1));
	}
	(void) waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "separate root process: FLINK/OPEN/mmap checks");
	check(p[1] == 0xfeedf00d, "the other process's write is visible here");
	p[1] = pattern(1, 0x5eed);

	(void) pipe(pc);
	(void) pipe(cp);
	pid = fork();
	if (pid == 0) {
		(void) close(pc[0]);
		(void) close(cp[1]);
		failures = 0;
		_exit(open_by_name(name, off, size, 0x5eed, 1, pc[1], cp[0]));
	}
	(void) close(pc[1]);
	(void) close(cp[0]);
	if (read(pc[0], &magic, sizeof (magic)) == sizeof (magic))
		check(drmAuthMagic(fd, magic) == 0, "master authenticates nobody's magic");
	(void) write(cp[1], &ok, 1);
	(void) waitpid(pid, &status, 0);
	(void) close(pc[0]);
	(void) close(cp[1]);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "unprivileged process: FLINK/OPEN checks");
	p[1] = pattern(1, 0x5eed);

	/* A second handle in the same file for the same object. */
	check(gem_open(fd, name, &h2, &sz) == 0 && h2 != h, "GEM_OPEN in the creating file gives a second handle");
	check(gem_stats(&g) && g.handles == 2 && g.objects == 1, "kstat: two handles, one object");

	/* Close the handles; the mapping keeps the object alive. */
	check(gem_close(fd, h, 1) == 0, "GEM_CLOSE of the first handle (pad ignored, as in Linux)");
	check(gem_close(fd, h, 0) == EINVAL, "GEM_CLOSE again is EINVAL");
	check(gem_open(fd, name, &h, &sz) == 0, "the name survives while a handle remains");
	check(gem_close(fd, h, 0) == 0 && destroy_dumb(fd, h2) == 0, "close the remaining handles");
	check(gem_open(fd, name, &h, &sz) == ENOENT, "the name is gone with the last handle");
	check(gem_stats(&g) && g.objects == 1 && g.handles == 0 && g.names == 0 && g.mappings == 1,
	    "kstat: the mapping keeps the object (1 object, 0 handles, 0 names)");
	bad = verify(p, size, 0x5eed);
	check(bad == (size_t)-1, "the mapping still shows the pattern after the handles are gone");
	e = munmap((void *)p, size);
	check(e == 0, "munmap");
	check(gem_idle("after the last munmap"), "the last munmap frees the object");
}

/* --- close(2) with objects, and the concurrency run --- */

static void
test_close_releases(void)
{
	uint32_t h, name;
	uint64_t off;
	uint32_t *p;
	int fd = open(CARD, O_RDWR);
	int i;

	check(fd >= 0, "open card0 for the close test");
	for (i = 0; i < 3; i++)
		(void) create_dumb(fd, 256, 256, 32, 0, &h, NULL, NULL);
	(void) gem_flink(fd, h, &name);
	(void) map_dumb(fd, h, 0, &off);
	p = map(fd, 256 * 256 * 4, MAP_SHARED, off);
	check(p != NULL, "mmap before close");
	(void) close(fd);
	check(p != NULL && p[0] == 0, "the mapping outlives close(2) of the file");
	check(!gem_idle("after close with a mapping"), "the mapping keeps its object after close");
	(void) munmap((void *)p, 256 * 256 * 4);
	check(gem_idle("after close and munmap"), "close(2) released three objects, a name and the handles");
}

struct worker {
	int id;
	int shared_fd;
	int errors;
	int rounds;
};

static void *
worker(void *arg)
{
	struct worker *w = arg;
	uint32_t h, h2, name, *p;
	uint64_t size, off, sz;
	int i, fd = open(CARD, O_RDWR);
	uint32_t keep = 0;

	if (fd < 0) {
		w->errors++;
		return (NULL);
	}
	for (i = 0; i < 150; i++) {
		uint32_t seed = (uint32_t)(w->id << 16 | i);
		if (create_dumb(fd, 64 + i % 7, 64, 32, 0, &h, NULL, &size) != 0 ||
		    map_dumb(fd, h, 0, &off) != 0) {
			w->errors++;
			continue;
		}
		p = map(fd, size, MAP_SHARED, off);
		if (p == NULL) {
			w->errors++;
		} else {
			fill(p, size, seed);
			if (verify(p, size, seed) != (size_t)-1)
				w->errors++;
		}
		/* Name it and open it through the shared file too. */
		if (gem_flink(fd, h, &name) != 0 || gem_open(w->shared_fd, name, &h2, &sz) != 0 ||
		    sz != size) {
			w->errors++;
		} else if (gem_close(w->shared_fd, h2, 0) != 0) {
			w->errors++;
		}
		if (p != NULL)
			(void) munmap((void *)p, size);
		if (i % 10 == 9) {
			/* Leave a handle behind for close(2) to release. */
			keep = h;
		} else if (destroy_dumb(fd, h) != 0) {
			w->errors++;
		}
		if (i % 50 == 49) {
			(void) close(fd);
			fd = open(CARD, O_RDWR);
			if (fd < 0) {
				w->errors++;
				return (NULL);
			}
		}
		w->rounds++;
	}
	(void) keep;
	(void) close(fd);
	return (NULL);
}

static void
test_concurrency(int fd)
{
	pthread_t t[8];
	struct worker w[8];
	int i, errors = 0, rounds = 0;
	char what[128];

	for (i = 0; i < 8; i++) {
		w[i].id = i;
		w[i].shared_fd = fd;
		w[i].errors = w[i].rounds = 0;
		(void) pthread_create(&t[i], NULL, worker, &w[i]);
	}
	for (i = 0; i < 8; i++) {
		(void) pthread_join(t[i], NULL);
		errors += w[i].errors;
		rounds += w[i].rounds;
	}
	(void) snprintf(what, sizeof (what),
	    "8 threads: create/map/mmap/write/flink/open/close/destroy, %d rounds, %d errors", rounds, errors);
	check(errors == 0 && rounds == 8 * 150, what);
	check(gem_idle("after the concurrency run"), "the concurrency run leaves nothing behind");
}

/* --- the refault experiment --- */

static int
move(int fd, uint32_t handle, uint32_t flags, uint32_t *pieces, uint32_t *backing)
{
	struct rdrmnull_move m;
	int e;

	(void) memset(&m, 0, sizeof (m));
	m.handle = handle;
	m.flags = flags;
	e = xioctl(fd, RDRMNULL_IOCTL_MOVE, &m);
	if (e == 0) {
		*pieces = m.pieces;
		*backing = m.backing;
	}
	return (e);
}

static void
test_refault(int fd)
{
	struct rdrmnull_create_movable c;
	uint32_t pieces, backing, dh;
	uint64_t off, size = 4 * PAGE;
	volatile uint8_t *p;
	struct gem g;
	int i;
	struct gem g0, g1;
	pid_t pid;
	int status, pc[2], cp[2];
	char ok = 1, v;

	(void) memset(&c, 0, sizeof (c));
	c.size = size;
	check(xioctl(fd, RDRMNULL_IOCTL_CREATE_MOVABLE, &c) == 0, "refault: create a movable object (two backings)");
	check(create_dumb(fd, 16, 16, 32, 0, &dh, NULL, NULL) == 0, "refault: create a dumb buffer");
	check(move(fd, dh, 0, &pieces, &backing) == EINVAL, "refault: RDRMNULL_IOCTL_MOVE of a dumb buffer is EINVAL");
	(void) destroy_dumb(fd, dh);
	check(map_dumb(fd, c.handle, 0, &off) == 0, "refault: MAP_DUMB works for any GEM object");
	p = (volatile uint8_t *)map(fd, size, MAP_SHARED, off);
	check(p != NULL, "refault: mmap the movable object");
	if (p == NULL)
		return;
	(void) memset((void *)p, 0x11, size);

	/* 1. Unload only: the next access refaults through devmap_access. */
	(void) gem_stats(&g0);
	check(move(fd, c.handle, MOVE_UNLOAD_ONLY, &pieces, &backing) == 0 && pieces == 1 && backing == 0,
	    "refault: devmap_unload of the live mapping (1 piece)");
	(void) gem_stats(&g1);
	check(g1.faults == g0.faults, "refault: no access, no fault yet");
	v = p[0];
	(void) gem_stats(&g1);
	(void) printf("INFO refault: faults %llu -> %llu after unload and one read\n",
	    (unsigned long long)g0.faults, (unsigned long long)g1.faults);
	check(v == 0x11 && g1.faults > g0.faults,
	    "refault: the read after devmap_unload faults into devmap_access and devmap_load reloads it");

	/* 2. Move to the other backing, filled with 0x22. */
	g0 = g1;
	check(move(fd, c.handle, MOVE_FILL | 0x22, &pieces, &backing) == 0 && pieces == 1 && backing == 1,
	    "refault: devmap_umem_remap to the second backing and devmap_unload (1 piece)");
	check(all_bytes((const void *)p, size, 0x22), "refault: the existing mapping shows the new backing (0x22)");
	(void) gem_stats(&g1);
	(void) printf("INFO refault: faults %llu -> %llu after the move and a full read\n",
	    (unsigned long long)g0.faults, (unsigned long long)g1.faults);
	check(g1.faults > g0.faults, "refault: the read after the move went through devmap_access");
	(void) memset((void *)p, 0x33, size);

	/* 3. Move back without fill: the first backing kept its contents. */
	check(move(fd, c.handle, 0, &pieces, &backing) == 0 && pieces == 1 && backing == 0,
	    "refault: move back to the first backing");
	check(all_bytes((const void *)p, size, 0x11), "refault: the first backing kept its contents (0x11)");

	/* 4. Two processes map the object; one move redirects both. */
	(void) pipe(pc);
	(void) pipe(cp);
	pid = fork();
	if (pid == 0) {
		(void) close(pc[0]);
		(void) close(cp[1]);
		failures = 0;
		check(all_bytes((const void *)p, size, 0x11), "refault child: inherited mapping shows 0x11");
		(void) write(pc[1], &ok, 1);
		(void) read(cp[0], &ok, 1);
		check(all_bytes((const void *)p, size, 0x33),
		    "refault child: after the parent's move the inherited mapping shows the second backing (0x33)");
		_exit(failures);
	}
	(void) close(pc[1]);
	(void) close(cp[0]);
	(void) read(pc[0], &ok, 1);
	check(move(fd, c.handle, 0, &pieces, &backing) == 0 && pieces == 2 && backing == 1,
	    "refault: one move redirects the mappings of both processes (2 pieces)");
	(void) write(cp[1], &ok, 1);
	(void) waitpid(pid, &status, 0);
	(void) close(pc[0]);
	(void) close(cp[1]);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "refault: child checks (see above)");
	check(all_bytes((const void *)p, size, 0x33), "refault: the parent sees the second backing too");

	/*
	 * 5. Partial munmap of a movable mapping. Page i holds 0x40 + i in the
	 * second backing and 0x50 + i in the first, so that a piece pointed at
	 * the wrong offset shows. The head and then the middle page are
	 * unmapped, leaving pieces for pages 1 and 3, which the framework
	 * finishes after the unmap callback; they must keep showing their own
	 * pages, and moves must redirect them, both before and after their
	 * first fault settles them.
	 */
	for (i = 0; i < 4; i++)
		(void) memset((void *)(p + i * PAGE), 0x40 + i, PAGE);
	check(move(fd, c.handle, 0, &pieces, &backing) == 0 && backing == 0, "refault: move to the first backing");
	for (i = 0; i < 4; i++)
		(void) memset((void *)(p + i * PAGE), 0x50 + i, PAGE);
	check(move(fd, c.handle, 0, &pieces, &backing) == 0 && backing == 1 &&
	    all_bytes((const void *)(p + PAGE), PAGE, 0x41) && all_bytes((const void *)(p + 3 * PAGE), PAGE, 0x43),
	    "refault: per-page patterns in both backings");
	check(munmap((void *)p, PAGE) == 0, "refault: munmap the head page of the movable mapping");
	check(munmap((void *)(p + 2 * PAGE), PAGE) == 0, "refault: munmap a middle page of the movable mapping");
	(void) gem_stats(&g);
	check(g.mappings == 2, "refault: two pieces remain (pages 1 and 3)");
	check(all_bytes((const void *)(p + PAGE), PAGE, 0x41) && all_bytes((const void *)(p + 3 * PAGE), PAGE, 0x43),
	    "refault: after the splits the pieces show their own pages (0x41, 0x43)");
	check(move(fd, c.handle, 0, &pieces, &backing) == 0 && pieces == 2 && backing == 0,
	    "refault: move with split pieces (2 pieces, invalidated only until their next fault)");
	check(all_bytes((const void *)(p + PAGE), PAGE, 0x51) && all_bytes((const void *)(p + 3 * PAGE), PAGE, 0x53),
	    "refault: the split pieces follow the move at their right offsets (0x51, 0x53)");
	check(move(fd, c.handle, MOVE_UNLOAD_ONLY, &pieces, &backing) == 0 && pieces == 2 &&
	    all_bytes((const void *)(p + PAGE), PAGE, 0x51) && all_bytes((const void *)(p + 3 * PAGE), PAGE, 0x53),
	    "refault: unload and refault of the settled pieces keeps their offsets");
	check(move(fd, c.handle, 0, &pieces, &backing) == 0 && pieces == 2 && backing == 1 &&
	    all_bytes((const void *)(p + PAGE), PAGE, 0x41) && all_bytes((const void *)(p + 3 * PAGE), PAGE, 0x43),
	    "refault: a second move remaps the settled pieces at their right offsets (0x41, 0x43)");

	(void) munmap((void *)(p + PAGE), PAGE);
	(void) munmap((void *)(p + 3 * PAGE), PAGE);
	check(gem_close(fd, c.handle, 0) == 0, "refault: close the movable object");
	check(gem_idle("after the refault experiment"), "refault: both backings are freed");
}

/* --- release from interrupt context --- */

static void
test_put_later(int fd)
{
	struct rdrmnull_put_later pl;
	struct gem g0, g1;
	uint32_t h;
	int i;

	check(create_dumb(fd, 64, 64, 32, 0, &h, NULL, NULL) == 0, "deferred: create a dumb buffer");
	(void) gem_stats(&g0);
	pl.handle = h;
	pl.delay_ms = 200;
	check(xioctl(fd, RDRMNULL_IOCTL_PUT_LATER, &pl) == 0, "deferred: a timer takes a reference");
	check(xioctl(fd, RDRMNULL_IOCTL_PUT_LATER, &pl) == EBUSY, "deferred: one timer at a time (EBUSY)");
	check(gem_close(fd, h, 0) == 0, "deferred: close the handle; the timer holds the last reference");
	(void) gem_stats(&g1);
	check(g1.objects == 1 && g1.handles == 0, "deferred: the object lives until the timer fires");
	for (i = 0; i < 50; i++) {
		msleep(100);
		(void) gem_stats(&g1);
		if (g1.objects == 0)
			break;
	}
	print_gem("after the timer", &g1);
	check(g1.objects == 0 && g1.umem_bytes == 0, "deferred: the object is freed after the timer fired");
	check(g1.deferred_frees == g0.deferred_frees + 1,
	    "deferred: the last reference was dropped in interrupt context and parked");
	check(g1.drain_tasks > g0.drain_tasks, "deferred: the task queue destroyed it in thread context");
	check(gem_idle("after the deferred release"), "deferred: nothing is left");
}

int
main(int argc, char **argv)
{
	int fd, rfd;

	(void) argc;
	(void) argv;
	check(gem_idle("at start"), "kstat rdrmnull:0:gem exists and is idle");
	fd = open(CARD, O_RDWR);
	check(fd >= 0, "open " CARD);
	if (fd < 0)
		return (failures);
	test_caps(fd, "card0");
	rfd = open(RENDER, O_RDWR);
	if (rfd >= 0) {
		test_caps(rfd, "renderD128");
		(void) close(rfd);
	}
	test_dumb_args(fd);
	test_memory_limit(fd);
	test_dumb(fd);
	test_close_releases();
	test_concurrency(fd);
	test_refault(fd);
	test_put_later(fd);
	(void) close(fd);
	check(gem_idle("at the end"), "every counter is back to zero");
	(void) printf("drm_r1_test: %d failures\n", failures);
	return (failures > 255 ? 255 : failures);
}

#endif	/* PROBE32 */
