/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The argument size rule of the core (G6): core and mode-setting commands
 * are served whatever size the caller's structure has, as Linux's
 * drm_ioctl() serves them (crates/drm/src/ioctl.rs). Run on any device
 * built on the core (rdrmnull, virtio_gpu):
 *
 *   drm_g6_test [card [render]]       (default $DRM_CARD, $DRM_RENDER,
 *                                      else card0 and renderD128)
 *   drm_g6_test libdrm [render]       one call of each grown sync object
 *                                      function of the installed libdrm, for
 *                                      truss(1): libdrm 2.4.134 must not
 *                                      retry at the 2.4.109 size
 *
 * The four commands whose structures grew between libdrm 2.4.109 (the
 * kernel's uAPI) and 2.4.134 (gfx-drm README.illumos) are called at both
 * sizes, and the results (return value, errno, output fields, the bytes
 * after the kernel's structure) must agree:
 *
 *   SYNCOBJ_HANDLE_TO_FD, FD_TO_HANDLE  drm_syncobj_handle   16 -> 24
 *   SYNCOBJ_WAIT                        drm_syncobj_wait      32 -> 40
 *   SYNCOBJ_TIMELINE_WAIT               drm_syncobj_timeline_wait 40 -> 48
 *
 * and the rest of the rule: a shorter structure is zero-extended and only
 * its bytes come back, a longer one of a command that only copies out
 * gets zeros after the kernel's structure, an unreadable byte of a longer
 * structure is EFAULT, another direction is ENOTTY.
 *
 * Build on illumos with the headers of tools/uapi/generate.sh in drm/:
 *   gcc -m64 -Wall -I drm -o drm_g6_test drm_g6_test.c \
 *       -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "drm.h"
#include <xf86drm.h>

/* The libdrm 2.4.109 layouts (the kernel's) and the 2.4.134 ones. */
struct handle_old {
	uint32_t handle, flags;
	int32_t fd, pad;
};
struct handle_new {
	uint32_t handle, flags;
	int32_t fd, pad;
	int64_t point;
};
struct wait_old {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
};
struct wait_new {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};
struct twait_old {
	uint64_t handles, points;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
};
struct twait_new {
	uint64_t handles, points;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};

typedef char assert_sizes[(sizeof (struct handle_old) == 16 && sizeof (struct handle_new) == 24 &&
    sizeof (struct wait_old) == 32 && sizeof (struct wait_new) == 40 &&
    sizeof (struct twait_old) == 40 && sizeof (struct twait_new) == 48 &&
    sizeof (struct drm_syncobj_handle) == 16 && sizeof (struct drm_syncobj_wait) == 32 &&
    sizeof (struct drm_syncobj_timeline_wait) == 40) ? 1 : -1];

#define	WAIT_ALL		(1 << 0)
#define	WAIT_FOR_SUBMIT		(1 << 1)
#define	WAIT_DEADLINE		(1 << 3)
#define	HANDLE_TIMELINE		(1 << 1)

/* The bytes 2.4.134 added, with a pattern the kernel must hand back. */
#define	TAIL	0x1122334455667788LL

static int failures, passes;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (ok)
		passes++;
	else
		failures++;
	(void) fflush(stdout);
}

/* ioctl(2) returning 0 or errno. */
static int
xioctl(int fd, unsigned long cmd, void *arg)
{
	return (ioctl(fd, cmd, arg) == 0 ? 0 : errno);
}

static unsigned long
sized(unsigned long cmd, size_t size)
{
	return ((cmd & ~((unsigned long)IOCPARM_MASK << 16)) | ((size & IOCPARM_MASK) << 16));
}

static uint32_t
create(int fd, int signaled)
{
	struct drm_syncobj_create c;

	(void) memset(&c, 0, sizeof (c));
	c.flags = signaled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0;
	return (xioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &c) == 0 ? c.handle : 0);
}

static void
destroy(int fd, uint32_t h)
{
	struct drm_syncobj_destroy d;

	(void) memset(&d, 0, sizeof (d));
	d.handle = h;
	(void) xioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
}

static int
fd_ok(int f)
{
	struct stat st;

	return (f >= 0 && fstat(f, &st) == 0);
}

/* SYNCOBJ_HANDLE_TO_FD and FD_TO_HANDLE at 16 and 24 bytes. */
static void
handles(int fd)
{
	uint32_t h = create(fd, 1), bad = 0x7fffffff;
	struct handle_old o;
	struct handle_new n;
	int eo, en, fo = -1, fn = -1;
	uint32_t ho = 0, hn = 0;

	check(h != 0, "SYNCOBJ_CREATE (signalled)");
	(void) memset(&o, 0, sizeof (o));
	o.handle = h;
	o.fd = -1;
	(void) memset(&n, 0, sizeof (n));
	n.handle = h;
	n.fd = -1;
	n.point = TAIL;
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 16), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 24), &n);
	(void) printf("INFO HANDLE_TO_FD: 16 bytes %d fd %d, 24 bytes %d fd %d\n", eo, o.fd, en, n.fd);
	check(eo == 0 && en == 0 && fd_ok(o.fd) && fd_ok(n.fd),
	    "HANDLE_TO_FD at 16 and 24 bytes: both export the sync object");
	check(n.point == TAIL && n.handle == h && n.flags == 0,
	    "HANDLE_TO_FD at 24 bytes: point comes back unchanged");
	fo = o.fd;
	fn = n.fd;

	(void) memset(&o, 0, sizeof (o));
	o.fd = fo;
	(void) memset(&n, 0, sizeof (n));
	n.fd = fn;
	n.point = TAIL;
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, 16), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, 24), &n);
	ho = o.handle;
	hn = n.handle;
	(void) printf("INFO FD_TO_HANDLE: 16 bytes %d handle %u, 24 bytes %d handle %u\n", eo, ho, en, hn);
	check(eo == 0 && en == 0 && ho != 0 && hn != 0 && ho != hn && n.point == TAIL,
	    "FD_TO_HANDLE at 16 and 24 bytes: both import, point unchanged");

	/* The same refusals at both sizes. */
	(void) memset(&o, 0, sizeof (o));
	o.handle = bad;
	(void) memset(&n, 0, sizeof (n));
	n.handle = bad;
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 16), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 24), &n);
	check(eo == EINVAL && en == EINVAL, "HANDLE_TO_FD of an unknown handle: EINVAL at both sizes");
	(void) memset(&n, 0, sizeof (n));
	n.handle = h;
	n.flags = HANDLE_TIMELINE;
	n.point = 1;
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 24), &n);
	check(en == EINVAL, "HANDLE_TO_FD_FLAGS_TIMELINE is an unknown flag (EINVAL), as before Linux had it");
	(void) memset(&o, 0, sizeof (o));
	o.fd = -1;
	(void) memset(&n, 0, sizeof (n));
	n.fd = -1;
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, 16), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, 24), &n);
	check(eo == en && eo != 0, "FD_TO_HANDLE of fd -1: the same error at both sizes");
	(void) printf("INFO FD_TO_HANDLE of -1: %s\n", strerror(eo));

	/* A shorter caller: 12 bytes, the fd field is the last one it has. */
	{
		unsigned char buf[24];
		int32_t f = -1;

		(void) memset(buf, 0xee, sizeof (buf));
		(void) memset(buf, 0, 12);
		(void) memcpy(buf, &h, sizeof (h));
		en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 12), buf);
		(void) memcpy(&f, buf + 8, sizeof (f));
		check(en == 0 && fd_ok(f) && buf[12] == 0xee && buf[23] == 0xee,
		    "HANDLE_TO_FD at 12 bytes: pad is zero-extended, 12 bytes come back");
		if (f >= 0)
			(void) close(f);
	}

	destroy(fd, ho);
	destroy(fd, hn);
	destroy(fd, h);
	(void) close(fo);
	(void) close(fn);
}

/* SYNCOBJ_WAIT at 32 and 40 bytes. */
static void
waits(int fd)
{
	uint32_t hs[2] = { create(fd, 0), create(fd, 1) };
	struct wait_old o;
	struct wait_new n;
	int eo, en;

	check(hs[0] != 0 && hs[1] != 0, "SYNCOBJ_CREATE (one unsignalled, one signalled)");
	/*
	 * hs[0] has no fence yet, which only WAIT_FOR_SUBMIT may wait for
	 * (EINVAL otherwise, as on Linux); hs[1] is signalled.
	 */
#define	SETW(w, flags_, timeout_)	do {				\
	(void) memset(&(w), 0, sizeof (w));				\
	(w).handles = (uintptr_t)hs;					\
	(w).count_handles = 2;						\
	(w).flags = (flags_);						\
	(w).timeout_nsec = (timeout_);					\
	(w).first_signaled = 0xdead;					\
} while (0)
	/* Any: the second one is signalled. */
	SETW(o, WAIT_FOR_SUBMIT, 0);
	SETW(n, WAIT_FOR_SUBMIT, 0);
	n.deadline_nsec = TAIL;
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 32), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 40), &n);
	(void) printf("INFO WAIT any: 32 bytes %d first %u, 40 bytes %d first %u\n", eo, o.first_signaled,
	    en, n.first_signaled);
	check(eo == 0 && en == 0 && o.first_signaled == 1 && n.first_signaled == 1 && n.deadline_nsec == TAIL,
	    "WAIT for any at 32 and 40 bytes: first_signaled 1 at both, deadline_nsec unchanged");
	/* All, polled: ETIME at both sizes. */
	SETW(o, WAIT_ALL | WAIT_FOR_SUBMIT, 0);
	SETW(n, WAIT_ALL | WAIT_FOR_SUBMIT, 0);
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 32), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 40), &n);
	check(eo == ETIME && en == ETIME, "WAIT for all, polled, with an unsubmitted object: ETIME at both sizes");
	/* Without WAIT_FOR_SUBMIT, an object without a fence is EINVAL. */
	SETW(o, 0, 0);
	SETW(n, 0, 0);
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 32), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 40), &n);
	check(eo == EINVAL && en == EINVAL, "WAIT on an object without a fence: EINVAL at both sizes");
	/* WAIT_FOR_SUBMIT on an object without a fence: ETIME too (100 ms). */
	{
		struct timespec ts;
		int64_t now;

		(void) clock_gettime(CLOCK_MONOTONIC, &ts);
		now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
		SETW(o, WAIT_ALL | WAIT_FOR_SUBMIT, now + 100000000LL);
		SETW(n, WAIT_ALL | WAIT_FOR_SUBMIT, now + 100000000LL);
		eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 32), &o);
		en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 40), &n);
		check(eo == ETIME && en == ETIME, "WAIT_FOR_SUBMIT with a 100 ms deadline: ETIME at both sizes");
	}
	/* The deadline flag is unknown to this kernel at either size. */
	SETW(n, WAIT_DEADLINE, 0);
	n.deadline_nsec = 1;
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 40), &n);
	check(en == EINVAL, "WAIT_FLAGS_WAIT_DEADLINE is an unknown flag (EINVAL), as before Linux had it");
	/* A shorter caller, 24 bytes: first_signaled and pad are zero-extended. */
	{
		unsigned char buf[40];

		SETW(o, 0, 0);
		o.handles = (uintptr_t)&hs[1];
		o.count_handles = 1;
		(void) memset(buf, 0xee, sizeof (buf));
		(void) memcpy(buf, &o, 24);
		en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 24), buf);
		check(en == 0 && buf[24] == 0xee && buf[31] == 0xee && buf[39] == 0xee,
		    "WAIT at 24 bytes: first_signaled and pad zero-extended, not copied back");
	}
	/* Another direction is another command. */
	SETW(o, 0, 0);
	check(xioctl(fd, (DRM_IOCTL_SYNCOBJ_WAIT & ~IOC_INOUT) | IOC_IN, &o) == ENOTTY,
	    "WAIT as _IOW (another direction) is ENOTTY");
	/*
	 * An unreadable byte after the kernel's structure: EFAULT, as Linux
	 * copies the caller's whole structure in.
	 */
	{
		long pg = sysconf(_SC_PAGESIZE);
		unsigned char *p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

		if (p != MAP_FAILED && munmap(p + pg, pg) == 0) {
			struct wait_old *w = (struct wait_old *)(void *)(p + pg - 32);
			SETW(*w, WAIT_FOR_SUBMIT, 0);
			eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 32), w);
			en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_WAIT, 40), w);
			check(eo == 0 && en == EFAULT,
			    "WAIT: 32 bytes before an unmapped page work, 40 bytes are EFAULT");
			(void) munmap(p, pg);
		} else {
			check(0, "mmap for the EFAULT check");
		}
	}
#undef SETW
	destroy(fd, hs[0]);
	destroy(fd, hs[1]);
}

/* SYNCOBJ_TIMELINE_WAIT at 40 and 48 bytes. */
static void
timeline_waits(int fd)
{
	uint32_t hs[2] = { create(fd, 0), create(fd, 1) };
	uint64_t pts[2] = { 0, 0 };
	struct twait_old o;
	struct twait_new n;
	int eo, en;

#define	SETT(w, flags_)	do {					\
	(void) memset(&(w), 0, sizeof (w));				\
	(w).handles = (uintptr_t)hs;					\
	(w).points = (uintptr_t)pts;					\
	(w).count_handles = 2;						\
	(w).flags = (flags_);						\
	(w).first_signaled = 0xdead;					\
} while (0)
	SETT(o, WAIT_FOR_SUBMIT);
	SETT(n, WAIT_FOR_SUBMIT);
	n.deadline_nsec = TAIL;
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 40), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 48), &n);
	(void) printf("INFO TIMELINE_WAIT any: 40 bytes %d first %u, 48 bytes %d first %u\n", eo,
	    o.first_signaled, en, n.first_signaled);
	check(eo == 0 && en == 0 && o.first_signaled == 1 && n.first_signaled == 1 && n.deadline_nsec == TAIL,
	    "TIMELINE_WAIT for any at 40 and 48 bytes: first_signaled 1 at both, deadline_nsec unchanged");
	SETT(o, WAIT_ALL | WAIT_FOR_SUBMIT);
	SETT(n, WAIT_ALL | WAIT_FOR_SUBMIT);
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 40), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 48), &n);
	check(eo == ETIME && en == ETIME, "TIMELINE_WAIT for all, polled: ETIME at both sizes");
	SETT(o, 0);
	SETT(n, 0);
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 40), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 48), &n);
	check(eo == EINVAL && en == EINVAL, "TIMELINE_WAIT on an object without a fence: EINVAL at both sizes");
	SETT(o, 1u << 7);
	SETT(n, 1u << 7);
	eo = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 40), &o);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 48), &n);
	check(eo == EINVAL && en == EINVAL, "TIMELINE_WAIT with an unknown flag: EINVAL at both sizes");
	SETT(n, WAIT_DEADLINE);
	en = xioctl(fd, sized(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 48), &n);
	check(en == EINVAL, "TIMELINE_WAIT with WAIT_DEADLINE: EINVAL (unknown flag)");
#undef SETT
	destroy(fd, hs[0]);
	destroy(fd, hs[1]);
}

/* Other commands: shorter and longer structures. */
static void
others(int card, int render)
{
	unsigned char buf[32];
	uint64_t v = 0;

	/* SYNCOBJ_CREATE with 4 bytes: flags arrive as 0, the handle comes back. */
	(void) memset(buf, 0xee, sizeof (buf));
	(void) memset(buf, 0, 4);
	{
		uint32_t h = 0;
		int e = xioctl(render, sized(DRM_IOCTL_SYNCOBJ_CREATE, 4), buf);
		(void) memcpy(&h, buf, sizeof (h));
		check(e == 0 && h != 0 && buf[4] == 0xee, "SYNCOBJ_CREATE at 4 bytes: a handle, 4 bytes back");
		if (h != 0)
			destroy(render, h);
	}
	/* GET_CAP with 16 bytes (exact) and 24 (8 more, copied back unchanged). */
	(void) memset(buf, 0xee, sizeof (buf));
	(void) memset(buf, 0, 16);
	buf[0] = DRM_CAP_SYNCOBJ;
	check(xioctl(render, sized(DRM_IOCTL_GET_CAP, 24), buf) == 0 && memcpy(&v, buf + 8, 8) != NULL && v == 1 &&
	    buf[16] == 0xee && buf[23] == 0xee && buf[24] == 0xee,
	    "GET_CAP at 24 bytes: SYNCOBJ 1, the 8 extra bytes come back unchanged");
	/* GET_CAP with 8 bytes: the value is not copied back. */
	(void) memset(buf, 0xee, sizeof (buf));
	(void) memset(buf, 0, 8);
	buf[0] = DRM_CAP_SYNCOBJ;
	check(xioctl(render, sized(DRM_IOCTL_GET_CAP, 8), buf) == 0 && buf[8] == 0xee && buf[15] == 0xee,
	    "GET_CAP at 8 bytes: served, the value is not copied back");
	/*
	 * GET_MAGIC only copies out (_IOR, 4 bytes): 8 bytes come back, the 4
	 * after the kernel's structure as zeros, as Linux's buffer is zeroed.
	 */
	(void) memset(buf, 0xee, sizeof (buf));
	check(xioctl(card, sized(DRM_IOCTL_GET_MAGIC, 8), buf) == 0 && buf[4] == 0 && buf[7] == 0 &&
	    buf[8] == 0xee, "GET_MAGIC (_IOR) at 8 bytes: the magic, then 4 zero bytes");
	/* Unknown numbers stay ENOTTY at any size. */
	check(xioctl(render, _IOWR('d', 0x3f, char[8]), buf) == ENOTTY, "an unused core number is ENOTTY");
}

/* One call of each grown libdrm function, for truss. */
static int
libdrm_calls(const char *node)
{
	uint32_t h = 0, h2 = 0;
	uint64_t point = 0;
	int fd = open(node, O_RDWR), ofd = -1;

	if (fd < 0) {
		perror(node);
		return (1);
	}
	check(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &h) == 0, "libdrm: drmSyncobjCreate");
	check(drmSyncobjHandleToFD(fd, h, &ofd) == 0 && ofd >= 0, "libdrm: drmSyncobjHandleToFD");
	check(drmSyncobjFDToHandle(fd, ofd, &h2) == 0 && h2 != 0, "libdrm: drmSyncobjFDToHandle");
	check(drmSyncobjWait(fd, &h, 1, INT64_MAX, 0, NULL) == 0, "libdrm: drmSyncobjWait");
	check(drmSyncobjTimelineWait(fd, &h, &point, 1, INT64_MAX, 0, NULL) == 0, "libdrm: drmSyncobjTimelineWait");
	(void) drmSyncobjDestroy(fd, h2);
	(void) drmSyncobjDestroy(fd, h);
	(void) close(ofd);
	(void) close(fd);
	(void) printf("drm_g6_test libdrm: %d passed, %d failed\n", passes, failures);
	return (failures != 0);
}

int
main(int argc, char **argv)
{
	const char *card = getenv("DRM_CARD"), *render = getenv("DRM_RENDER");
	int cfd, rfd;
	drmVersionPtr v;

	if (card == NULL || *card == '\0')
		card = "/dev/dri/card0";
	if (render == NULL || *render == '\0')
		render = "/dev/dri/renderD128";
	if (argc > 1 && strcmp(argv[1], "libdrm") == 0)
		return (libdrm_calls(argc > 2 ? argv[2] : render));
	if (argc > 1)
		card = argv[1];
	if (argc > 2)
		render = argv[2];
	(void) printf("nodes under test: %s %s\n", card, render);
	if ((cfd = open(card, O_RDWR)) < 0 || (rfd = open(render, O_RDWR)) < 0) {
		perror("open");
		return (1);
	}
	if ((v = drmGetVersion(rfd)) != NULL) {
		(void) printf("driver %s %d.%d.%d\n", v->name, v->version_major, v->version_minor,
		    v->version_patchlevel);
		drmFreeVersion(v);
	}
	handles(rfd);
	waits(rfd);
	timeline_waits(rfd);
	others(cfd, rfd);
	(void) close(rfd);
	(void) close(cfd);
	(void) printf("drm_g6_test: %d passed, %d failed\n", passes, failures);
	return (failures != 0);
}
