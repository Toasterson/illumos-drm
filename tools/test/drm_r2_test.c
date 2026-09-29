/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Userland test of the illumos-drm R2 core (fences, sync objects with
 * timelines, sync files, PRIME with anonymous file descriptors), run as
 * root against rdrmnull.
 *
 * The positive paths use the installed libdrm 2.4.109 (drmPrime*,
 * drmSyncobj*); the error paths use raw ioctl(2), so that the errno the
 * kernel returns is checked exactly (drmIoctl restarts on EINTR). Fences
 * come from rdrmnull's test timeline (RDRMNULL_IOCTL_FENCE), which a
 * timeout(9F) callback and a soft interrupt signal after a delay. The
 * `sync` and `gem` kstats (rdrmnull:0:sync, rdrmnull:0:gem) are read with
 * libkstat to check that every fence, callback, sync object, sync file,
 * dma-buf and anonymous file is gone at the end.
 *
 * Two-process parts fork and exec this program again ("child" mode), so
 * that the child has nothing but the descriptors it receives with
 * SCM_RIGHTS over a socket pair (and close-on-exec descriptors are
 * checked to be gone).
 *
 * Every check prints PASS or FAIL; the exit status is the number of
 * failures (capped at 255 by the shell).
 *
 * Build on illumos, with the patched libdrm headers in $INC:
 *   gcc -m64 -Wall -I$INC -o drm_r2_test drm_r2_test.c \
 *       -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread -lsocket
 * and the 32-bit probe (no libdrm):
 *   gcc -m32 -Wall -DPROBE32 -I$INC -o drm_r2_probe32 drm_r2_test.c
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "drm.h"
#include "drm_mode.h"

#define	CARD	"/dev/dri/card0"
#define	RENDER	"/dev/dri/renderD128"

/* The test ioctls of rdrmnull (examples/rdrmnull/src/lib.rs). */
struct rdrmnull_fence {
	uint32_t delay_ms;
	uint32_t flags;
	uint32_t syncobj;
	uint32_t bo;
	uint64_t point;
	int32_t fd;
	int32_t error;
	uint64_t seqno;
};
struct rdrmnull_bo_wait {
	uint32_t handle;
	uint32_t flags;
	int64_t timeout_ns;
};
_Static_assert(sizeof (struct rdrmnull_fence) == 40, "layout");
_Static_assert(sizeof (struct rdrmnull_bo_wait) == 16, "layout");
#define	RDRMNULL_IOCTL_FENCE DRM_IOWR(DRM_COMMAND_BASE + 0x03, struct rdrmnull_fence)
#define	RDRMNULL_IOCTL_BO_WAIT DRM_IOW(DRM_COMMAND_BASE + 0x04, struct rdrmnull_bo_wait)
#define	FENCE_SYNCOBJ	(1u << 0)
#define	FENCE_BO_WRITE	(1u << 1)
#define	FENCE_BO_READ	(1u << 2)
#define	FENCE_SYNC_FILE	(1u << 3)
#define	BO_WAIT_WRITERS	(1u << 0)

/* linux/sync_file.h and linux/dma-buf.h, in the illumos encoding. */
struct sync_merge_data {
	char name[32];
	int32_t fd2;
	int32_t fence;
	uint32_t flags;
	uint32_t pad;
};
struct sync_fence_info {
	char obj_name[32];
	char driver_name[32];
	int32_t status;
	uint32_t flags;
	uint64_t timestamp_ns;
};
struct sync_file_info {
	char name[32];
	int32_t status;
	uint32_t flags;
	uint32_t num_fences;
	uint32_t pad;
	uint64_t sync_fence_info;
};
struct dma_buf_sync {
	uint64_t flags;
};
#define	SYNC_IOC_MERGE		_IOWR('>', 3, struct sync_merge_data)
#define	SYNC_IOC_FILE_INFO	_IOWR('>', 4, struct sync_file_info)
#define	DMA_BUF_IOCTL_SYNC	_IOW('b', 0, struct dma_buf_sync)
#define	DMA_BUF_SYNC_READ	1
#define	DMA_BUF_SYNC_WRITE	2
#define	DMA_BUF_SYNC_RW		3
#define	DMA_BUF_SYNC_END	4

#ifndef DRM_RDWR
#define	DRM_RDWR	O_RDWR
#endif

static int failures;
static int passes;

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

static int
xioctl_err(int fd, unsigned long cmd, void *arg)
{
	errno = 0;
	if (ioctl(fd, cmd, arg) == 0)
		return (0);
	return (errno);
}

#ifdef PROBE32

/* 32-bit processes get ENOTSUP for the R2 core ioctls. */
int
main(void)
{
	struct drm_syncobj_create sc;
	struct drm_prime_handle ph;
	int fd = open(RENDER, O_RDWR);

	check(fd >= 0, "32-bit open " RENDER);
	if (fd < 0)
		return (1);
	(void) memset(&sc, 0, sizeof (sc));
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc) == ENOTSUP,
	    "32-bit SYNCOBJ_CREATE fails with ENOTSUP");
	(void) memset(&ph, 0, sizeof (ph));
	check(xioctl_err(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == ENOTSUP,
	    "32-bit PRIME_HANDLE_TO_FD fails with ENOTSUP");
	(void) close(fd);
	(void) printf("probe32: %d failures\n", failures);
	return (failures);
}

#else	/* PROBE32 */

#include <kstat.h>
#include <xf86drm.h>

static const char *self_path;

/* --- time --- */

static int64_t
now_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
}

static int64_t
ms_ns(int64_t ms)
{
	return (ms * 1000000LL);
}

/* --- kstats --- */

static int
kstat_values(const char *name, const char *const *keys, uint64_t *out, int n)
{
	kstat_ctl_t *kc = kstat_open();
	kstat_t *ks;
	int i;

	if (kc == NULL)
		return (-1);
	ks = kstat_lookup(kc, "rdrmnull", 0, (char *)name);
	if (ks == NULL || kstat_read(kc, ks, NULL) == -1) {
		(void) kstat_close(kc);
		return (-1);
	}
	for (i = 0; i < n; i++) {
		kstat_named_t *kn = kstat_data_lookup(ks, (char *)keys[i]);
		out[i] = kn == NULL ? UINT64_MAX : kn->value.ui64;
	}
	(void) kstat_close(kc);
	return (0);
}

static const char *const sync_live_keys[] = {
	"fences", "callbacks", "arrays", "chains", "syncobjs", "sync_files",
	"dmabufs", "anon_files"
};
#define	NLIVE	(sizeof (sync_live_keys) / sizeof (sync_live_keys[0]))

/* Waits up to two seconds for the live sync counters to reach zero. */
static int
sync_idle(int print)
{
	uint64_t v[NLIVE];
	int tries, i, busy = 1;

	for (tries = 0; tries < 40 && busy; tries++) {
		if (kstat_values("sync", sync_live_keys, v, NLIVE) != 0)
			return (0);
		busy = 0;
		for (i = 0; i < (int)NLIVE; i++)
			busy |= v[i] != 0;
		if (busy)
			(void) usleep(50000);
	}
	if (print) {
		(void) printf("  sync kstat:");
		for (i = 0; i < (int)NLIVE; i++)
			(void) printf(" %s=%llu", sync_live_keys[i],
			    (unsigned long long)v[i]);
		(void) printf("\n");
	}
	return (!busy);
}

static uint64_t
sync_counter(const char *key)
{
	uint64_t v = UINT64_MAX;
	const char *k[1] = { key };

	(void) kstat_values("sync", k, &v, 1);
	return (v);
}

/* --- helpers --- */

static int
open_node(const char *path)
{
	/* Close-on-exec, so that the exec'd children inherit nothing. */
	return (open(path, O_RDWR | O_CLOEXEC));
}

static uint32_t
dumb(int fd, uint32_t w, uint32_t h, uint64_t *size)
{
	struct drm_mode_create_dumb cd;

	(void) memset(&cd, 0, sizeof (cd));
	cd.width = w;
	cd.height = h;
	cd.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0)
		return (0);
	if (size != NULL)
		*size = cd.size;
	return (cd.handle);
}

static void *
map_handle(int fd, uint32_t h, size_t size)
{
	struct drm_mode_map_dumb md;
	void *p;

	(void) memset(&md, 0, sizeof (md));
	md.handle = h;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0)
		return (NULL);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)md.offset);
	return (p == MAP_FAILED ? NULL : p);
}

static void
gem_close(int fd, uint32_t h)
{
	struct drm_gem_close gc;

	(void) memset(&gc, 0, sizeof (gc));
	gc.handle = h;
	(void) drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
}

static int
fill_check(const volatile uint32_t *p, size_t words, uint32_t seed)
{
	size_t i;

	for (i = 0; i < words; i++)
		if (p[i] != (uint32_t)(seed ^ (i * 2654435761u)))
			return (0);
	return (1);
}

static void
fill(volatile uint32_t *p, size_t words, uint32_t seed)
{
	size_t i;

	for (i = 0; i < words; i++)
		p[i] = (uint32_t)(seed ^ (i * 2654435761u));
}

/* A fence from the rdrmnull timeline; returns 0 or an errno. */
static int
null_fence(int fd, uint32_t delay_ms, uint32_t flags, uint32_t syncobj,
    uint64_t point, uint32_t bo, int error, int *sync_fd)
{
	struct rdrmnull_fence f;
	int r;

	(void) memset(&f, 0, sizeof (f));
	f.delay_ms = delay_ms;
	f.flags = flags;
	f.syncobj = syncobj;
	f.point = point;
	f.bo = bo;
	f.error = error;
	f.fd = -1;
	r = xioctl_err(fd, RDRMNULL_IOCTL_FENCE, &f);
	if (sync_fd != NULL)
		*sync_fd = f.fd;
	return (r);
}

static int
is_cloexec(int fd)
{
	int f = fcntl(fd, F_GETFD);

	return (f != -1 && (f & FD_CLOEXEC) != 0);
}

/* --- descriptor passing --- */

static int
send_fds(int sock, const int *fds, int n)
{
	struct msghdr msg;
	struct iovec iov;
	char byte = 'x';
	char buf[CMSG_SPACE(sizeof (int) * 4)];
	struct cmsghdr *cm;

	(void) memset(&msg, 0, sizeof (msg));
	(void) memset(buf, 0, sizeof (buf));
	iov.iov_base = &byte;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = CMSG_SPACE(sizeof (int) * n);
	cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof (int) * n);
	(void) memcpy(CMSG_DATA(cm), fds, sizeof (int) * n);
	return (sendmsg(sock, &msg, 0) == 1 ? 0 : -1);
}

static int
recv_fds(int sock, int *fds, int n)
{
	struct msghdr msg;
	struct iovec iov;
	char byte;
	char buf[CMSG_SPACE(sizeof (int) * 4)];
	struct cmsghdr *cm;

	(void) memset(&msg, 0, sizeof (msg));
	iov.iov_base = &byte;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof (buf);
	if (recvmsg(sock, &msg, 0) != 1)
		return (-1);
	cm = CMSG_FIRSTHDR(&msg);
	if (cm == NULL || cm->cmsg_type != SCM_RIGHTS ||
	    cm->cmsg_len != CMSG_LEN(sizeof (int) * n))
		return (-1);
	(void) memcpy(fds, CMSG_DATA(cm), sizeof (int) * n);
	return (0);
}

static int
send_word(int sock, uint32_t w)
{
	return (write(sock, &w, sizeof (w)) == sizeof (w) ? 0 : -1);
}

static int
recv_word(int sock, uint32_t *w)
{
	return (read(sock, w, sizeof (*w)) == sizeof (*w) ? 0 : -1);
}

/* Starts this program again as a child that talks over *sock. */
static pid_t
spawn_child(const char *mode, int *sock)
{
	int sv[2];
	pid_t pid;
	char arg[16];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return (-1);
	pid = fork();
	if (pid == 0) {
		(void) close(sv[0]);
		(void) snprintf(arg, sizeof (arg), "%d", sv[1]);
		(void) execl(self_path, self_path, mode, arg, (char *)NULL);
		_exit(127);
	}
	(void) close(sv[1]);
	*sock = sv[0];
	return (pid);
}

static int
reap(pid_t pid)
{
	int st;

	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
		return (-1);
	return (WEXITSTATUS(st));
}

/* ------------------------------------------------------------------ */
/* Capabilities                                                        */
/* ------------------------------------------------------------------ */

static void
test_caps(int card)
{
	uint64_t v = 0;

	check(drmGetCap(card, DRM_CAP_PRIME, &v) == 0 &&
	    v == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT),
	    "GET_CAP PRIME reports import and export");
	check(drmGetCap(card, DRM_CAP_SYNCOBJ, &v) == 0 && v == 1,
	    "GET_CAP SYNCOBJ is 1");
	check(drmGetCap(card, DRM_CAP_SYNCOBJ_TIMELINE, &v) == 0 && v == 1,
	    "GET_CAP SYNCOBJ_TIMELINE is 1");
}

/* ------------------------------------------------------------------ */
/* PRIME                                                               */
/* ------------------------------------------------------------------ */

#define	PW	256
#define	PH	256

/*
 * Child side of the PRIME test: receives a dma-buf, imports it on its
 * own card and render files, maps it both through the handle and through
 * the dma-buf itself, checks the parent's pattern and writes its own.
 */
static int
prime_child(int sock)
{
	int fds[1], card, render, n, stray = 0;
	uint32_t h, h2, hr, seed, sz32, kept;
	size_t size;
	volatile uint32_t *p, *q;
	struct stat s1, s2;

	if (recv_fds(sock, fds, 1) != 0 || recv_word(sock, &seed) != 0 ||
	    recv_word(sock, &sz32) != 0 || recv_word(sock, &kept) != 0)
		return (100);
	size = sz32;
	/*
	 * Of the parent's descriptors only the export without close-on-exec
	 * (number `kept`) survived the exec; the received one is new.
	 */
	for (n = 3; n < 64; n++) {
		if (n != sock && n != fds[0] && n != (int)kept &&
		    fcntl(n, F_GETFD) != -1) {
			(void) printf("child: unexpected open descriptor %d\n",
			    n);
			stray++;
		}
	}
	check(stray == 0, "child: close-on-exec descriptors did not survive exec");
	check(fstat((int)kept, &s1) == 0 && fstat(fds[0], &s2) == 0 &&
	    s1.st_rdev == s2.st_rdev,
	    "child: the inherited export and the received one are one dma-buf");
	(void) close((int)kept);
	card = open_node(CARD);
	render = open_node(RENDER);
	check(card >= 0 && render >= 0, "child: open card0 and renderD128");
	check(drmPrimeFDToHandle(card, fds[0], &h) == 0 && h != 0,
	    "child: PRIME_FD_TO_HANDLE imports the received dma-buf");
	check(drmPrimeFDToHandle(card, fds[0], &h2) == 0 && h2 == h,
	    "child: importing again returns the same handle");
	check(drmPrimeFDToHandle(render, fds[0], &hr) == 0 && hr != 0,
	    "child: the render node imports it too");
	p = map_handle(card, h, size);
	check(p != NULL, "child: MAP_DUMB and mmap of the imported handle");
	if (p != NULL)
		check(fill_check(p, size / 4, seed),
		    "child: the imported buffer holds the parent's pattern");
	q = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
	check(q != MAP_FAILED, "child: mmap of the dma-buf descriptor");
	if (q != MAP_FAILED) {
		check(fill_check(q, size / 4, seed),
		    "child: the dma-buf mapping holds the parent's pattern");
		fill(q, size / 4, seed ^ 0xa5a5a5a5u);
		check(p == NULL || fill_check(p, size / 4, seed ^ 0xa5a5a5a5u),
		    "child: a write through the dma-buf shows in the handle mapping");
		(void) munmap((void *)q, size);
	}
	if (p != NULL)
		(void) munmap((void *)p, size);
	gem_close(card, h);
	gem_close(render, hr);
	(void) close(fds[0]);
	(void) close(card);
	(void) close(render);
	(void) send_word(sock, 1);
	return (failures);
}

static void
test_prime(int card, int render)
{
	uint64_t size = 0;
	uint32_t h, h2, hr, got;
	int pfd = -1, pfd2 = -1, rofd = -1, sock, st;
	volatile uint32_t *p;
	void *m;
	struct stat s1, s2;
	struct drm_prime_handle ph;
	struct dma_buf_sync dbs;
	pid_t pid;
	uint32_t seed = 0x12345678;

	h = dumb(card, PW, PH, &size);
	check(h != 0, "PRIME: create a dumb buffer");
	p = map_handle(card, h, size);
	check(p != NULL, "PRIME: map it");
	if (h == 0 || p == NULL)
		return;
	fill(p, size / 4, seed);

	check(drmPrimeHandleToFD(card, h, DRM_CLOEXEC | DRM_RDWR, &pfd) == 0 &&
	    pfd >= 0, "PRIME_HANDLE_TO_FD exports it (CLOEXEC|RDWR)");
	check(is_cloexec(pfd), "the dma-buf descriptor is close-on-exec");
	check(drmPrimeHandleToFD(card, h, 0, &pfd2) == 0 && pfd2 >= 0 &&
	    !is_cloexec(pfd2), "exporting again without CLOEXEC");
	check(fstat(pfd, &s1) == 0 && fstat(pfd2, &s2) == 0 &&
	    S_ISCHR(s1.st_mode) && s1.st_rdev == s2.st_rdev,
	    "both exports are the same dma-buf (one device number)");
	check(drmPrimeFDToHandle(card, pfd, &got) == 0 && got == h,
	    "PRIME_FD_TO_HANDLE on the exporting file returns its handle");
	check(drmPrimeFDToHandle(render, pfd, &hr) == 0 && hr != 0,
	    "the render node imports it as a new handle");
	check(drmPrimeFDToHandle(render, pfd2, &h2) == 0 && h2 == hr,
	    "importing the second export gives the same handle");

	/* Read-only export: no writable shared mapping. */
	check(drmPrimeHandleToFD(card, h, DRM_CLOEXEC, &rofd) == 0,
	    "export read-only (no DRM_RDWR)");
	m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, rofd, 0);
	check(m == MAP_FAILED && errno == EACCES,
	    "a writable mapping of a read-only dma-buf fails with EACCES");
	m = mmap(NULL, size, PROT_READ, MAP_SHARED, rofd, 0);
	check(m != MAP_FAILED && fill_check(m, size / 4, seed),
	    "a read-only mapping of it shows the contents");
	if (m != MAP_FAILED)
		(void) munmap(m, size);
	m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, pfd, 0);
	check(m == MAP_FAILED, "a private mapping of a dma-buf is refused");
	m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, pfd, (off_t)size);
	check(m == MAP_FAILED, "a mapping beyond the dma-buf is refused");
	m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, pfd, 4096);
	check(m != MAP_FAILED &&
	    ((volatile uint32_t *)m)[0] == (seed ^ (1024 * 2654435761u)),
	    "a mapping at a page offset shows that page");
	if (m != MAP_FAILED)
		(void) munmap(m, 4096);

	/* DMA_BUF_IOCTL_SYNC and other ioctls on the dma-buf. */
	dbs.flags = DMA_BUF_SYNC_RW;
	check(xioctl_err(pfd, DMA_BUF_IOCTL_SYNC, &dbs) == 0,
	    "DMA_BUF_IOCTL_SYNC start RW succeeds");
	dbs.flags = DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END;
	check(xioctl_err(pfd, DMA_BUF_IOCTL_SYNC, &dbs) == 0,
	    "DMA_BUF_IOCTL_SYNC end RW succeeds");
	dbs.flags = 0;
	check(xioctl_err(pfd, DMA_BUF_IOCTL_SYNC, &dbs) == EINVAL,
	    "DMA_BUF_IOCTL_SYNC without read or write is EINVAL");
	dbs.flags = 8 | DMA_BUF_SYNC_READ;
	check(xioctl_err(pfd, DMA_BUF_IOCTL_SYNC, &dbs) == EINVAL,
	    "DMA_BUF_IOCTL_SYNC with an unknown flag is EINVAL");
	check(xioctl_err(pfd, DRM_IOCTL_VERSION, &dbs) == ENOTTY,
	    "a DRM ioctl on a dma-buf is ENOTTY");

	/* Error paths. */
	(void) memset(&ph, 0, sizeof (ph));
	ph.handle = h;
	ph.flags = 0x4;
	check(xioctl_err(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == EINVAL,
	    "PRIME_HANDLE_TO_FD with an unknown flag is EINVAL");
	ph.handle = 0x7777;
	ph.flags = 0;
	check(xioctl_err(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == ENOENT,
	    "PRIME_HANDLE_TO_FD of an unknown handle is ENOENT");
	(void) memset(&ph, 0, sizeof (ph));
	ph.fd = 9999;
	check(xioctl_err(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == EBADF,
	    "PRIME_FD_TO_HANDLE of a closed descriptor is EBADF");
	ph.fd = 0;
	check(xioctl_err(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == EINVAL,
	    "PRIME_FD_TO_HANDLE of a non-dma-buf descriptor is EINVAL");
	ph.fd = card;
	check(xioctl_err(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == EINVAL,
	    "PRIME_FD_TO_HANDLE of a DRM file is EINVAL");
	ph.fd = pfd;
	ph.flags = 1;
	check(xioctl_err(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == EINVAL,
	    "PRIME_FD_TO_HANDLE with flags is EINVAL");

	/* The two-process round trip. */
	pid = spawn_child("prime-child", &sock);
	check(pid > 0, "start the PRIME child");
	if (pid > 0) {
		uint32_t done = 0;

		check(send_fds(sock, &pfd, 1) == 0 && send_word(sock, seed) == 0 &&
		    send_word(sock, (uint32_t)size) == 0 &&
		    send_word(sock, (uint32_t)pfd2) == 0,
		    "send the dma-buf over SCM_RIGHTS");
		check(recv_word(sock, &done) == 0 && done == 1,
		    "the child finished");
		st = reap(pid);
		check(st == 0, "the child passed all its checks");
		check(fill_check(p, size / 4, seed ^ 0xa5a5a5a5u),
		    "the parent sees the child's writes");
		(void) close(sock);
	}

	/* A dma-buf keeps the object alive after its handles are gone. */
	gem_close(render, hr);
	(void) munmap((void *)p, size);
	gem_close(card, h);
	m = mmap(NULL, size, PROT_READ, MAP_SHARED, pfd, 0);
	check(m != MAP_FAILED &&
	    fill_check(m, size / 4, seed ^ 0xa5a5a5a5u),
	    "the dma-buf maps the object after every handle is closed");
	(void) close(pfd);
	(void) close(pfd2);
	(void) close(rofd);
	check(m == MAP_FAILED ||
	    fill_check(m, size / 4, seed ^ 0xa5a5a5a5u),
	    "the mapping outlives the descriptors");
	if (m != MAP_FAILED)
		(void) munmap(m, size);
	check(sync_idle(1), "PRIME: every dma-buf and anonymous file is gone");
}

/* ------------------------------------------------------------------ */
/* Sync objects                                                        */
/* ------------------------------------------------------------------ */

static void
test_syncobj_basic(int fd)
{
	uint32_t a = 0, b = 0, first = 99;
	uint32_t hs[2];
	struct drm_syncobj_create sc;
	struct drm_syncobj_destroy sd;
	struct drm_syncobj_wait sw;
	struct drm_syncobj_array sa;
	int64_t t0;

	check(drmSyncobjCreate(fd, 0, &a) == 0 && a != 0, "SYNCOBJ_CREATE");
	check(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &b) == 0 &&
	    b != 0 && b != a, "SYNCOBJ_CREATE signalled");
	hs[0] = a;
	hs[1] = b;
	(void) memset(&sw, 0, sizeof (sw));
	sw.handles = (uintptr_t)&hs[1];
	sw.count_handles = 1;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == 0,
	    "WAIT with a zero timeout on a signalled object succeeds");
	sw.handles = (uintptr_t)&hs[0];
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == EINVAL,
	    "WAIT on an object without a fence is EINVAL");
	sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == ETIME,
	    "WAIT_FOR_SUBMIT with a zero timeout is ETIME");
	t0 = now_ns();
	sw.timeout_nsec = now_ns() + ms_ns(100);
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == ETIME &&
	    now_ns() - t0 >= ms_ns(90), "WAIT_FOR_SUBMIT times out at its deadline");
	check(drmSyncobjWait(fd, hs, 2, 0, 0, &first) == -EINVAL,
	    "libdrm drmSyncobjWait reports EINVAL for the empty one");
	check(drmSyncobjSignal(fd, &a, 1) == 0, "SYNCOBJ_SIGNAL");
	check(drmSyncobjWait(fd, hs, 2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
	    &first) == 0, "WAIT_ALL on two signalled objects");
	check(drmSyncobjReset(fd, &a, 1) == 0, "SYNCOBJ_RESET");
	check(drmSyncobjWait(fd, hs, 2, 0, 0, &first) == -EINVAL,
	    "a reset object has no fence again");
	check(drmSyncobjWait(fd, hs, 2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
	    &first) == 0 && first == 1, "WAIT any returns the signalled one first");

	/* Argument checks. */
	(void) memset(&sc, 0, sizeof (sc));
	sc.flags = 2;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc) == EINVAL,
	    "CREATE with an unknown flag is EINVAL");
	(void) memset(&sd, 0, sizeof (sd));
	sd.handle = a;
	sd.pad = 1;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd) == EINVAL,
	    "DESTROY with pad set is EINVAL");
	sd.pad = 0;
	sd.handle = 0x5555;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd) == EINVAL,
	    "DESTROY of an unknown handle is EINVAL");
	(void) memset(&sw, 0, sizeof (sw));
	sw.handles = (uintptr_t)hs;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == EINVAL,
	    "WAIT with no handles is EINVAL");
	sw.count_handles = 1;
	sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == EINVAL,
	    "WAIT_AVAILABLE is only for TIMELINE_WAIT (EINVAL)");
	hs[1] = 0x5555;
	sw.flags = 0;
	sw.count_handles = 2;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == ENOENT,
	    "WAIT on an unknown handle is ENOENT");
	sw.handles = 8;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == EFAULT,
	    "WAIT with an unreadable handle array is EFAULT");
	(void) memset(&sa, 0, sizeof (sa));
	sa.handles = (uintptr_t)&a;
	sa.count_handles = 1;
	sa.pad = 1;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_RESET, &sa) == EINVAL,
	    "RESET with pad set is EINVAL");
	sa.pad = 0;
	sa.count_handles = 0;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &sa) == EINVAL,
	    "SIGNAL of no handles is EINVAL");
	sa.count_handles = 70000;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &sa) == EINVAL,
	    "SIGNAL of more than 65536 handles is EINVAL");
	check(drmSyncobjDestroy(fd, a) == 0 && drmSyncobjDestroy(fd, b) == 0,
	    "SYNCOBJ_DESTROY both");
}

static void
test_timeline(int fd)
{
	uint32_t t = 0, bin = 0, first;
	uint64_t pts[3] = { 3, 5, 9 }, q = 0;
	uint32_t hs[3];
	struct drm_syncobj_timeline_array ta;
	struct drm_syncobj_timeline_wait tw;
	struct drm_syncobj_transfer tr;
	int64_t t0;

	check(drmSyncobjCreate(fd, 0, &t) == 0 && drmSyncobjCreate(fd, 0, &bin) == 0,
	    "TIMELINE: create two objects");
	hs[0] = hs[1] = hs[2] = t;
	check(drmSyncobjTimelineSignal(fd, hs, pts, 1) == 0,
	    "TIMELINE_SIGNAL point 3");
	check(drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 3, "QUERY is 3");
	/* A fence of the null driver at point 5, signalled after 200 ms. */
	check(null_fence(fd, 200, FENCE_SYNCOBJ, t, 5, 0, 0, NULL) == 0,
	    "rdrmnull fence at point 5 (200 ms)");
	(void) memset(&ta, 0, sizeof (ta));
	ta.handles = (uintptr_t)&t;
	ta.points = (uintptr_t)&q;
	ta.count_handles = 1;
	ta.flags = DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_QUERY, &ta) == 0 && q == 5,
	    "QUERY LAST_SUBMITTED is 5");
	check(drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 3,
	    "QUERY is still 3 before the fence signals");
	check(drmSyncobjTimelineWait(fd, &t, &pts[1], 1, 0,
	    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, &first) == 0,
	    "TIMELINE_WAIT_AVAILABLE for point 5 returns at once");
	check(drmSyncobjTimelineWait(fd, &t, &pts[1], 1, 0, 0, &first) == -ETIME,
	    "TIMELINE_WAIT for point 5 with a zero timeout is ETIME");
	t0 = now_ns();
	check(drmSyncobjTimelineWait(fd, &t, &pts[1], 1, now_ns() + ms_ns(5000),
	    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, &first) == 0,
	    "TIMELINE_WAIT for point 5 returns when the soft interrupt signals it");
	check(now_ns() - t0 >= ms_ns(100),
	    "...after the fence's delay");
	check(drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 5, "QUERY is 5");

	/* Points out of order: 9 signals before 7 is submitted. */
	check(null_fence(fd, 300, FENCE_SYNCOBJ, t, 7, 0, 0, NULL) == 0,
	    "rdrmnull fence at point 7 (300 ms)");
	check(null_fence(fd, 50, FENCE_SYNCOBJ, t, 9, 0, 0, NULL) == 0,
	    "rdrmnull fence at point 9 (50 ms)");
	(void) usleep(150000);
	check(drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 5,
	    "QUERY stays 5 while point 7 is pending, though 9 signalled");
	check(drmSyncobjTimelineWait(fd, &t, &pts[2], 1, now_ns() + ms_ns(5000),
	    0, &first) == 0 && drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 9,
	    "point 9 is reached when 7 signals; QUERY is 9");

	/* WAIT_FOR_SUBMIT on a point that nobody has submitted yet. */
	q = 12;
	check(drmSyncobjTimelineWait(fd, &t, &q, 1, 0,
	    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &first) == -ETIME,
	    "WAIT_FOR_SUBMIT for point 12 polls with ETIME");
	check(drmSyncobjTimelineWait(fd, &t, &q, 1, 0, 0, &first) == -EINVAL,
	    "without WAIT_FOR_SUBMIT an unsubmitted point is EINVAL");

	/* TRANSFER both ways. */
	check(drmSyncobjTransfer(fd, bin, 0, t, 9, 0) == 0,
	    "TRANSFER point 9 into a binary object");
	check(drmSyncobjWait(fd, &bin, 1, 0, 0, &first) == 0,
	    "the binary object is signalled");
	check(null_fence(fd, 100, FENCE_SYNCOBJ, bin, 0, 0, 0, NULL) == 0,
	    "rdrmnull fence into the binary object (100 ms)");
	check(drmSyncobjTransfer(fd, t, 20, bin, 0, 0) == 0,
	    "TRANSFER the binary fence to point 20");
	pts[0] = 20;
	check(drmSyncobjTimelineWait(fd, &t, &pts[0], 1, now_ns() + ms_ns(5000),
	    0, &first) == 0 && drmSyncobjQuery(fd, &t, &q, 1) == 0 && q == 20,
	    "point 20 signals with the transferred fence; QUERY is 20");
	(void) memset(&tr, 0, sizeof (tr));
	tr.src_handle = t;
	tr.dst_handle = 0x4444;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &tr) == ENOENT,
	    "TRANSFER to an unknown handle is ENOENT");
	tr.dst_handle = bin;
	tr.pad = 1;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &tr) == EINVAL,
	    "TRANSFER with pad set is EINVAL");
	tr.pad = 0;
	tr.flags = 0x80;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &tr) == EINVAL,
	    "TRANSFER with an unknown flag is EINVAL");
	tr.flags = 0;
	tr.src_point = 99;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &tr) == EINVAL,
	    "TRANSFER of an unsubmitted point is EINVAL");

	/* Argument checks of the timeline ioctls. */
	(void) memset(&tw, 0, sizeof (tw));
	tw.handles = (uintptr_t)&t;
	tw.points = (uintptr_t)pts;
	tw.count_handles = 1;
	tw.flags = 8;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw) == EINVAL,
	    "TIMELINE_WAIT with an unknown flag is EINVAL");
	(void) memset(&ta, 0, sizeof (ta));
	ta.handles = (uintptr_t)&t;
	ta.points = (uintptr_t)pts;
	ta.count_handles = 1;
	ta.flags = 1;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &ta) == EINVAL,
	    "TIMELINE_SIGNAL with flags is EINVAL");
	ta.flags = 2;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_QUERY, &ta) == EINVAL,
	    "QUERY with an unknown flag is EINVAL");
	ta.flags = 0;
	ta.points = 8;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_QUERY, &ta) == EFAULT,
	    "QUERY into an unwritable array is EFAULT");
	check(drmSyncobjDestroy(fd, t) == 0 && drmSyncobjDestroy(fd, bin) == 0,
	    "TIMELINE: destroy");
}

/* ------------------------------------------------------------------ */
/* Sync files                                                          */
/* ------------------------------------------------------------------ */

static int
sync_status(int sfd, uint32_t *nfences)
{
	struct sync_file_info info;

	(void) memset(&info, 0, sizeof (info));
	if (ioctl(sfd, SYNC_IOC_FILE_INFO, &info) != 0)
		return (-1000);
	if (nfences != NULL)
		*nfences = info.num_fences;
	return (info.status);
}

static void
test_sync_file(int fd)
{
	int s1 = -1, s2 = -1, s3 = -1, ex = -1, r;
	uint32_t so = 0, empty = 0, n = 0, first;
	struct pollfd pf;
	struct sync_merge_data md;
	struct sync_file_info info;
	struct sync_fence_info fi[4];
	struct drm_syncobj_handle sh;
	int64_t t0;

	check(null_fence(fd, 300, FENCE_SYNC_FILE, 0, 0, 0, 0, &s1) == 0 &&
	    s1 >= 0, "rdrmnull fence as a sync file (300 ms)");
	check(is_cloexec(s1), "the sync file is close-on-exec");
	pf.fd = s1;
	pf.events = POLLIN;
	pf.revents = 0;
	check(poll(&pf, 1, 0) == 0, "poll(2) of the pending sync file: not ready");
	check(sync_status(s1, &n) == 0 && n == 1,
	    "SYNC_IOC_FILE_INFO: active, one fence");
	t0 = now_ns();
	r = poll(&pf, 1, 5000);
	check(r == 1 && (pf.revents & POLLIN) != 0,
	    "poll(2) wakes with POLLIN when the soft interrupt signals the fence");
	check(now_ns() - t0 >= ms_ns(150) && now_ns() - t0 < ms_ns(4000),
	    "...after the fence's delay, not at the timeout");
	check(sync_status(s1, NULL) == 1, "SYNC_IOC_FILE_INFO: signalled");

	/* An error fence reports its error. */
	check(null_fence(fd, 10, FENCE_SYNC_FILE, 0, 0, 0, EIO, &s2) == 0,
	    "rdrmnull fence with EIO (10 ms)");
	pf.fd = s2;
	check(poll(&pf, 1, 5000) == 1, "poll(2) wakes for the error fence");
	check(sync_status(s2, NULL) == -EIO, "SYNC_IOC_FILE_INFO status is -EIO");
	(void) close(s2);

	/* MERGE: signals when both have. */
	check(null_fence(fd, 100, FENCE_SYNC_FILE, 0, 0, 0, 0, &s2) == 0 &&
	    null_fence(fd, 400, FENCE_SYNC_FILE, 0, 0, 0, 0, &s3) == 0,
	    "two more sync files (100 ms, 400 ms)");
	(void) memset(&md, 0, sizeof (md));
	md.fence = -1;
	(void) strlcpy(md.name, "merged", sizeof (md.name));
	md.fd2 = s3;
	check(xioctl_err(s2, SYNC_IOC_MERGE, &md) == 0 && md.fence >= 0,
	    "SYNC_IOC_MERGE");
	check(is_cloexec(md.fence), "the merged sync file is close-on-exec");
	(void) memset(&info, 0, sizeof (info));
	(void) memset(fi, 0, sizeof (fi));
	info.num_fences = 4;
	info.sync_fence_info = (uintptr_t)fi;
	check(xioctl_err(md.fence, SYNC_IOC_FILE_INFO, &info) == 0 &&
	    info.num_fences == 2 && strcmp(info.name, "merged") == 0 &&
	    strcmp(fi[0].driver_name, "rdrmnull") == 0,
	    "FILE_INFO of the merge: two fences, its name, the driver name");
	info.num_fences = 1;
	check(xioctl_err(md.fence, SYNC_IOC_FILE_INFO, &info) == EINVAL,
	    "FILE_INFO with too little room is EINVAL");
	info.num_fences = 0;
	info.flags = 1;
	check(xioctl_err(md.fence, SYNC_IOC_FILE_INFO, &info) == EINVAL,
	    "FILE_INFO with flags is EINVAL");
	pf.fd = md.fence;
	t0 = now_ns();
	check(poll(&pf, 1, 5000) == 1 && now_ns() - t0 >= ms_ns(250),
	    "the merged sync file signals with the later fence");
	md.fd2 = fd;
	check(xioctl_err(s2, SYNC_IOC_MERGE, &md) == ENOENT,
	    "MERGE with a non-sync-file is ENOENT");
	md.fd2 = s3;
	md.flags = 1;
	check(xioctl_err(s2, SYNC_IOC_MERGE, &md) == EINVAL,
	    "MERGE with flags is EINVAL");
	check(xioctl_err(s2, DRM_IOCTL_VERSION, &md) == ENOTTY,
	    "a DRM ioctl on a sync file is ENOTTY");

	/* Sync objects and sync files. */
	check(drmSyncobjCreate(fd, 0, &so) == 0 && drmSyncobjCreate(fd, 0, &empty) == 0,
	    "two sync objects");
	check(drmSyncobjExportSyncFile(fd, empty, &ex) != 0,
	    "EXPORT_SYNC_FILE of an empty object fails");
	(void) memset(&sh, 0, sizeof (sh));
	sh.handle = empty;
	sh.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == EINVAL,
	    "...with EINVAL");
	check(null_fence(fd, 200, FENCE_SYNC_FILE, 0, 0, 0, 0, &ex) == 0,
	    "a sync file (200 ms)");
	check(drmSyncobjImportSyncFile(fd, so, ex) == 0,
	    "IMPORT_SYNC_FILE into a sync object");
	(void) close(ex);
	ex = -1;
	check(drmSyncobjWait(fd, &so, 1, now_ns() + ms_ns(5000), 0, &first) == 0,
	    "WAIT on the object with the imported fence");
	check(null_fence(fd, 200, FENCE_SYNCOBJ, so, 0, 0, 0, NULL) == 0,
	    "rdrmnull fence into the object (200 ms)");
	check(drmSyncobjExportSyncFile(fd, so, &ex) == 0 && ex >= 0,
	    "EXPORT_SYNC_FILE of it");
	pf.fd = ex;
	pf.events = POLLIN;
	check(poll(&pf, 1, 0) == 0 && poll(&pf, 1, 5000) == 1,
	    "poll(2) of the exported sync file waits for the fence");
	(void) memset(&sh, 0, sizeof (sh));
	sh.fd = ex;
	sh.handle = 0x6666;
	sh.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sh) == ENOENT,
	    "IMPORT_SYNC_FILE into an unknown handle is ENOENT");
	sh.handle = so;
	sh.fd = fd;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sh) == EINVAL,
	    "IMPORT_SYNC_FILE of a non-sync-file is EINVAL");
	sh.flags = 3;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sh) == EINVAL,
	    "FD_TO_HANDLE with an unknown flag is EINVAL");
	sh.flags = 0;
	sh.pad = 1;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == EINVAL,
	    "HANDLE_TO_FD with pad set is EINVAL");
	sh.pad = 0;
	sh.fd = ex;
	check(xioctl_err(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sh) == EINVAL,
	    "FD_TO_HANDLE of a sync file (not a sync object file) is EINVAL");

	(void) close(s1);
	(void) close(s2);
	(void) close(s3);
	(void) close(md.fence);
	(void) close(ex);
	(void) drmSyncobjDestroy(fd, so);
	(void) drmSyncobjDestroy(fd, empty);
	check(sync_idle(1), "sync files: everything is gone");
}

/* ------------------------------------------------------------------ */
/* Sync objects and sync files between processes                       */
/* ------------------------------------------------------------------ */

static int
syncobj_child(int sock)
{
	int fds[2], render;
	uint32_t h = 0, first, go;
	struct pollfd pf;

	if (recv_fds(sock, fds, 2) != 0)
		return (100);
	render = open_node(RENDER);
	check(render >= 0, "child: open renderD128");
	check(drmSyncobjFDToHandle(render, fds[0], &h) == 0 && h != 0,
	    "child: SYNCOBJ_FD_TO_HANDLE imports the parent's sync object");
	(void) close(fds[0]);
	check(send_word(sock, 1) == 0, "child: ready");
	check(drmSyncobjWait(render, &h, 1, now_ns() + ms_ns(10000),
	    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &first) == 0,
	    "child: WAIT_FOR_SUBMIT sees the parent's submission and its signal");
	pf.fd = fds[1];
	pf.events = POLLIN;
	check(poll(&pf, 1, 10000) == 1 && (pf.revents & POLLIN) != 0,
	    "child: poll(2) of the received sync file wakes");
	(void) close(fds[1]);
	check(recv_word(sock, &go) == 0 && go == 2, "child: timeline step");
	{
		uint64_t q = 0;

		check(drmSyncobjQuery(render, &h, &q, 1) == 0 && q == 42,
		    "child: QUERY sees the parent's TIMELINE_SIGNAL of point 42");
	}
	(void) drmSyncobjDestroy(render, h);
	(void) close(render);
	(void) send_word(sock, 3);
	return (failures);
}

static void
test_syncobj_processes(int fd)
{
	uint32_t so = 0, w = 0;
	int ofd = -1, sfd = -1, sock, st, fds[2];
	pid_t pid;
	uint64_t pt = 42;

	check(drmSyncobjCreate(fd, 0, &so) == 0, "PROCESSES: create a sync object");
	check(drmSyncobjHandleToFD(fd, so, &ofd) == 0 && ofd >= 0,
	    "SYNCOBJ_HANDLE_TO_FD exports it");
	check(is_cloexec(ofd), "the sync object descriptor is close-on-exec");
	check(null_fence(fd, 600, FENCE_SYNC_FILE, 0, 0, 0, 0, &sfd) == 0,
	    "a sync file for the child (600 ms)");
	pid = spawn_child("syncobj-child", &sock);
	check(pid > 0, "start the sync object child");
	if (pid <= 0)
		return;
	fds[0] = ofd;
	fds[1] = sfd;
	check(send_fds(sock, fds, 2) == 0,
	    "send the sync object and the sync file over SCM_RIGHTS");
	check(recv_word(sock, &w) == 0 && w == 1, "the child imported it");
	(void) usleep(100000);
	check(null_fence(fd, 100, FENCE_SYNCOBJ, so, 0, 0, 0, NULL) == 0,
	    "submit an rdrmnull fence to the shared object (100 ms)");
	check(drmSyncobjWait(fd, &so, 1, now_ns() + ms_ns(5000), 0, NULL) == 0,
	    "the fence signals");
	check(drmSyncobjTimelineSignal(fd, &so, &pt, 1) == 0,
	    "TIMELINE_SIGNAL point 42 of the shared object");
	(void) send_word(sock, 2);
	check(recv_word(sock, &w) == 0 && w == 3, "the child finished");
	st = reap(pid);
	check(st == 0, "the sync object child passed all its checks");
	(void) close(sock);
	(void) close(ofd);
	(void) close(sfd);
	(void) drmSyncobjDestroy(fd, so);
	check(sync_idle(1), "PROCESSES: everything is gone");
}

/* ------------------------------------------------------------------ */
/* Reservations                                                        */
/* ------------------------------------------------------------------ */

static int
bo_wait(int fd, uint32_t h, uint32_t flags, int64_t timeout)
{
	struct rdrmnull_bo_wait w;

	(void) memset(&w, 0, sizeof (w));
	w.handle = h;
	w.flags = flags;
	w.timeout_ns = timeout;
	return (xioctl_err(fd, RDRMNULL_IOCTL_BO_WAIT, &w));
}

static void
test_resv(int card)
{
	uint32_t h;
	int64_t t0;

	h = dumb(card, 64, 64, NULL);
	check(h != 0, "RESV: a buffer");
	check(bo_wait(card, h, 0, 0) == 0, "an idle buffer does not wait");
	check(null_fence(card, 200, FENCE_BO_WRITE, 0, 0, h, 0, NULL) == 0,
	    "a writer fence on it (200 ms)");
	check(bo_wait(card, h, BO_WAIT_WRITERS, 0) == ETIME,
	    "a zero-timeout wait for the writer is ETIME");
	t0 = now_ns();
	check(bo_wait(card, h, BO_WAIT_WRITERS, ms_ns(5000)) == 0 &&
	    now_ns() - t0 >= ms_ns(100),
	    "the wait for the writer returns when it signals");
	check(null_fence(card, 300, FENCE_BO_READ, 0, 0, h, 0, NULL) == 0,
	    "a reader fence (300 ms)");
	check(bo_wait(card, h, BO_WAIT_WRITERS, 0) == 0,
	    "readers do not hold up a wait for writers");
	check(bo_wait(card, h, 0, 0) == ETIME, "they hold up a wait for all");
	check(bo_wait(card, h, 0, ms_ns(5000)) == 0, "until they signal");
	check(null_fence(card, 10, FENCE_BO_READ | FENCE_BO_WRITE, 0, 0, h, 0,
	    NULL) == EINVAL, "a fence cannot be both writer and reader (EINVAL)");
	check(null_fence(card, 10, FENCE_BO_WRITE, 0, 0, 0x7777, 0, NULL) == ENOENT,
	    "a fence on an unknown buffer is ENOENT");
	check(bo_wait(card, h, 2, 0) == EINVAL, "BO_WAIT with an unknown flag");
	gem_close(card, h);
}

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

static void
on_alarm(int sig)
{
	(void) sig;
}

static void
test_eintr(int fd)
{
	struct sigaction sa;
	struct drm_syncobj_wait sw;
	uint32_t so = 0;
	int r;

	(void) memset(&sa, 0, sizeof (sa));
	sa.sa_handler = on_alarm;
	(void) sigaction(SIGALRM, &sa, NULL);
	check(drmSyncobjCreate(fd, 0, &so) == 0, "EINTR: a sync object");
	(void) memset(&sw, 0, sizeof (sw));
	sw.handles = (uintptr_t)&so;
	sw.count_handles = 1;
	sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	sw.timeout_nsec = now_ns() + ms_ns(10000);
	(void) alarm(1);
	r = xioctl_err(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw);
	(void) alarm(0);
	check(r == EINTR, "a signal interrupts a sync object wait with EINTR");
	(void) drmSyncobjDestroy(fd, so);
}

/* ------------------------------------------------------------------ */
/* Concurrency                                                         */
/* ------------------------------------------------------------------ */

#define	THREADS	8
#define	ROUNDS	60

static int thread_errors;
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void
terr(const char *what, int t, int i)
{
	(void) pthread_mutex_lock(&thread_lock);
	if (thread_errors++ < 10)
		(void) printf("  thread %d round %d: %s (errno %d)\n", t, i, what,
		    errno);
	(void) pthread_mutex_unlock(&thread_lock);
}

static void *
worker(void *arg)
{
	int t = (int)(intptr_t)arg, i, fd, pfd, sfd, ofd;
	uint32_t so, so2, h, h2, first;
	uint64_t pt;

	fd = open_node(t % 2 ? RENDER : CARD);
	if (fd < 0) {
		terr("open", t, -1);
		return (NULL);
	}
	for (i = 0; i < ROUNDS; i++) {
		pt = (uint64_t)i + 1;
		if (drmSyncobjCreate(fd, 0, &so) != 0) {
			terr("create", t, i);
			continue;
		}
		if (null_fence(fd, (uint32_t)(i % 7), FENCE_SYNCOBJ | FENCE_SYNC_FILE,
		    so, pt, 0, 0, &sfd) != 0)
			terr("fence", t, i);
		if (drmSyncobjHandleToFD(fd, so, &ofd) != 0 ||
		    drmSyncobjFDToHandle(fd, ofd, &so2) != 0)
			terr("syncobj fd round trip", t, i);
		else {
			(void) close(ofd);
			if (drmSyncobjTimelineWait(fd, &so2, &pt, 1,
			    now_ns() + ms_ns(10000), 0, &first) != 0)
				terr("timeline wait", t, i);
			(void) drmSyncobjDestroy(fd, so2);
		}
		{
			struct pollfd pf = { sfd, POLLIN, 0 };

			if (poll(&pf, 1, 10000) != 1)
				terr("sync file poll", t, i);
		}
		(void) close(sfd);
		(void) drmSyncobjDestroy(fd, so);
		if (t % 2 == 0) {
			h = dumb(fd, 32, 32, NULL);
			if (h == 0 || drmPrimeHandleToFD(fd, h, DRM_CLOEXEC, &pfd) != 0) {
				terr("export", t, i);
			} else {
				if (drmPrimeFDToHandle(fd, pfd, &h2) != 0 || h2 != h)
					terr("reimport", t, i);
				(void) close(pfd);
			}
			gem_close(fd, h);
		}
	}
	(void) close(fd);
	return (NULL);
}

static void
test_threads(void)
{
	pthread_t tids[THREADS];
	int i;

	for (i = 0; i < THREADS; i++)
		(void) pthread_create(&tids[i], NULL, worker, (void *)(intptr_t)i);
	for (i = 0; i < THREADS; i++)
		(void) pthread_join(tids[i], NULL);
	check(thread_errors == 0, "8 threads: sync objects, fences, sync files, "
	    "PRIME round trips concurrently");
	check(sync_idle(1), "THREADS: everything is gone");
}

int
main(int argc, char **argv)
{
	int card, render;
	uint64_t sig0, sig1;

	self_path = argv[0];
	if (argc == 3 && strcmp(argv[1], "prime-child") == 0)
		return (prime_child(atoi(argv[2])));
	if (argc == 3 && strcmp(argv[1], "syncobj-child") == 0)
		return (syncobj_child(atoi(argv[2])));

	card = open_node(CARD);
	render = open_node(RENDER);
	check(card >= 0 && render >= 0, "open card0 and renderD128");
	if (card < 0 || render < 0)
		return (1);
	sig0 = sync_counter("signaled");
	test_caps(card);
	test_prime(card, render);
	test_syncobj_basic(render);
	test_syncobj_basic(card);
	test_timeline(render);
	test_sync_file(render);
	test_syncobj_processes(card);
	test_resv(card);
	test_eintr(render);
	test_threads();
	(void) close(card);
	(void) close(render);
	check(sync_idle(1), "at the end the sync kstat shows nothing live");
	sig1 = sync_counter("signaled");
	(void) printf("fences signalled during the test: %llu\n",
	    (unsigned long long)(sig1 - sig0));
	(void) printf("drm_r2_test: %d passed, %d failed\n", passes, failures);
	return (failures);
}

#endif	/* PROBE32 */
