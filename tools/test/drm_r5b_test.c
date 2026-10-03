/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The userland half of the test of illumos-drm R5b (public-layout base
 * objects) on cdrmnull. Run as root while cdrmnull is attached:
 *
 *	DRM_CARD=/dev/dri/cardN ./drm_r5b_test [token [print]]
 *
 *  1. FENCE_OPS_SELFTEST: the fence checks inside the driver (fences with
 *     driver ops, the GPU-like timeline, deferred releases, the
 *     interrupt-level race), one line per check.
 *  2. Fences the core did not allocate (cdrmnull's timeline fences, with
 *     enable_signaling and signaled ops, signalled from a soft interrupt)
 *     in the core's structures: a binary sync object wait, an array (a
 *     merged sync file of two such fences), a chain (two points of a
 *     timeline sync object, signalled out of order), an error through a
 *     sync file, and releases parked by the core (kstat drm:0:sync
 *     "deferred").
 *  3. HANDLES: drm_illumos_gem_for_each_handle() over the file's handles
 *     (counts, lookups from inside the walk, an early stop), the GEM
 *     object's live reference count as the driver reads it (handle, name,
 *     dma-buf, mapping) with a kref_get_unless_zero() round trip, and walks
 *     while another thread creates and closes handles.
 *  4. PRINT_SELFTEST with `token` (hex; default from the time): a driver
 *     printer formats "<token> 42 abc -7"; the messages it logs are
 *     checked by run-r5b-test.sh in the system log.
 *
 *	gcc -m64 -Wall -I drm -I<illumos-drm>/drivers/cdrmnull \
 *	    -o drm_r5b_test drm_r5b_test.c \
 *	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <kstat.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm.h"
#include "drm_mode.h"
#include "cdrmnull_ioctl.h"

/* linux/sync_file.h, in the illumos encoding (as drm_r2_test.c). */
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
#define	SYNC_IOC_MERGE		_IOWR('>', 3, struct sync_merge_data)
#define	SYNC_IOC_FILE_INFO	_IOWR('>', 4, struct sync_file_info)

static int failures, passes;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	(void) fflush(stdout);
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

static int64_t
now_ms(void)
{
	return ((int64_t)(gethrtime() / 1000000));
}

static uint32_t
dumb(int fd, uint64_t *size)
{
	struct drm_mode_create_dumb c;

	(void) memset(&c, 0, sizeof (c));
	c.width = 64;
	c.height = 16;
	c.bpp = 32;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0)
		return (0);
	if (size != NULL)
		*size = c.size;
	return (c.handle);
}

static int
gem_close(int fd, uint32_t h)
{
	struct drm_gem_close c;

	(void) memset(&c, 0, sizeof (c));
	c.handle = h;
	return (xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c));
}

static uint64_t
sync_counter(const char *name)
{
	kstat_ctl_t *kc = kstat_open();
	kstat_t *ks;
	kstat_named_t *kn;
	uint64_t v = UINT64_MAX;

	if (kc == NULL)
		return (v);
	if ((ks = kstat_lookup(kc, "drm", 0, "sync")) != NULL &&
	    kstat_read(kc, ks, NULL) != -1 &&
	    (kn = kstat_data_lookup(ks, (char *)name)) != NULL)
		v = kn->value.ui64;
	(void) kstat_close(kc);
	return (v);
}

/* A fence of cdrmnull's timeline: FENCE with the given flags. */
static int
fence(int fd, uint32_t delay_ms, uint32_t flags, uint32_t syncobj,
    uint64_t point, int error, int *sync_fd)
{
	struct cdrmnull_fence f;
	int r;

	(void) memset(&f, 0, sizeof (f));
	f.delay_ms = delay_ms;
	f.flags = flags;
	f.syncobj = syncobj;
	f.point = point;
	f.error = error;
	f.fd = -1;
	r = xioctl(fd, DRM_IOCTL_CDRMNULL_FENCE, &f);
	if (r == 0 && sync_fd != NULL)
		*sync_fd = f.fd;
	return (r);
}

static void
test_ops_selftest(int fd)
{
	static const char *const names[] = {
		"ENABLE: enable_signaling once, not at init",
		"ENABLE_FALSE: enable_signaling false signals the fence",
		"POLL: ops->signaled is polled and signals",
		"WAIT_OP: dma_fence_wait_timeout calls ops->wait",
		"DEFAULT_WAIT: timeline fence waits (timeout, then signal)",
		"POLL_ONLY: completion without an interrupt-level signal",
		"DEFERRED: the soft interrupt's last put is released in thread "
		    "context",
		"QUEUED: last puts from a callback and under the lock are "
		    "queued",
		"EDEADLK: released with a callback: -EDEADLK",
		"BY_HAND: i915-style signalling reaches a sync object",
		"NAMES: driver and timeline names, set_deadline",
		"SEQNO64: dma_fence_init64 and the comparisons",
		"LOCKED: callbacks run with the fence's lock held",
		"STRESS: add/remove/signal race with the soft interrupt",
		"PROXY: a relay signals a second structure",
	};
	struct cdrmnull_fence_ops_selftest t;
	uint32_t so = 0, i;
	char what[160];

	if (drmSyncobjCreate(fd, 0, &so) != 0) {
		check(0, "FENCE_OPS_SELFTEST: create a sync object");
		return;
	}
	(void) memset(&t, 0, sizeof (t));
	t.syncobj = so;
	t.rounds = 200;
	check(xioctl(fd, DRM_IOCTL_CDRMNULL_FENCE_OPS_SELFTEST, &t) == 0,
	    "FENCE_OPS_SELFTEST runs");
	(void) printf("FENCE_OPS_SELFTEST passed 0x%x of 0x%x\n", t.passed,
	    CDRMNULL_OPS_T_ALL);
	for (i = 0; i < sizeof (names) / sizeof (names[0]); i++) {
		(void) snprintf(what, sizeof (what), "fence ops: %s", names[i]);
		check((t.passed & (1u << i)) != 0, what);
	}
	t.pad = 1;
	check(xioctl(fd, DRM_IOCTL_CDRMNULL_FENCE_OPS_SELFTEST, &t) == EINVAL,
	    "FENCE_OPS_SELFTEST with pad set is EINVAL");
	(void) drmSyncobjDestroy(fd, so);
}

static void
test_core_structures(int fd)
{
	uint32_t so = 0, tl = 0;
	int a = -1, b = -1, e = -1;
	int64_t t0, dt;
	uint64_t deferred0, point = 0;
	struct sync_merge_data m;
	struct sync_file_info info;
	struct sync_fence_info fi[2];
	struct pollfd p;
	int r;

	deferred0 = sync_counter("deferred");

	/* A binary sync object holding a timeline fence. */
	check(drmSyncobjCreate(fd, 0, &so) == 0, "create a binary sync object");
	t0 = now_ms();
	check(fence(fd, 60, CDRMNULL_FENCE_SYNCOBJ, so, 0, 0, NULL) == 0,
	    "timeline fence (60 ms) into the sync object");
	r = drmSyncobjWait(fd, &so, 1, INT64_MAX, 0, NULL);
	dt = now_ms() - t0;
	check(r == 0 && dt >= 50, "SYNCOBJ_WAIT returns when the soft "
	    "interrupt signals the driver's fence");
	(void) printf("    waited %" PRId64 " ms\n", dt);

	/* An array: two fences as sync files, merged. */
	t0 = now_ms();
	check(fence(fd, 40, CDRMNULL_FENCE_SYNC_FILE, 0, 0, 0, &a) == 0 &&
	    fence(fd, 90, CDRMNULL_FENCE_SYNC_FILE, 0, 0, 0, &b) == 0,
	    "two timeline fences (40, 90 ms) as sync files");
	(void) memset(&m, 0, sizeof (m));
	(void) strlcpy(m.name, "r5b-merge", sizeof (m.name));
	m.fd2 = b;
	m.fence = -1;
	check(xioctl(a, SYNC_IOC_MERGE, &m) == 0 && m.fence >= 0,
	    "SYNC_IOC_MERGE makes an array of the two");
	p.fd = m.fence;
	p.events = POLLIN;
	p.revents = 0;
	r = poll(&p, 1, 2000);
	dt = now_ms() - t0;
	check(r == 1 && (p.revents & POLLIN) != 0 && dt >= 80,
	    "the merged sync file polls ready when both have signalled");
	(void) printf("    merged ready after %" PRId64 " ms\n", dt);
	(void) memset(&info, 0, sizeof (info));
	(void) memset(fi, 0, sizeof (fi));
	info.num_fences = 2;
	info.sync_fence_info = (uint64_t)(uintptr_t)fi;
	check(xioctl(m.fence, SYNC_IOC_FILE_INFO, &info) == 0 &&
	    info.num_fences == 2 && info.status == 1 && fi[0].status == 1 &&
	    fi[1].status == 1, "FILE_INFO of the array: two members, "
	    "signalled");
	(void) close(m.fence);
	(void) close(a);
	(void) close(b);

	/* An error through a sync file. */
	check(fence(fd, 10, CDRMNULL_FENCE_SYNC_FILE, 0, 0, EIO, &e) == 0,
	    "timeline fence with EIO as a sync file");
	p.fd = e;
	p.events = POLLIN;
	(void) poll(&p, 1, 2000);
	(void) memset(&info, 0, sizeof (info));
	check(xioctl(e, SYNC_IOC_FILE_INFO, &info) == 0 &&
	    info.status == -EIO, "the sync file reports -EIO");
	(void) close(e);

	/* A chain: two points of a timeline, signalled out of order. */
	check(drmSyncobjCreate(fd, 0, &tl) == 0, "create a timeline sync "
	    "object");
	t0 = now_ms();
	check(fence(fd, 120, CDRMNULL_FENCE_SYNCOBJ, tl, 1, 0, NULL) == 0 &&
	    fence(fd, 20, CDRMNULL_FENCE_SYNCOBJ, tl, 2, 0, NULL) == 0,
	    "timeline fences at points 1 (120 ms) and 2 (20 ms)");
	point = 2;
	r = drmSyncobjTimelineWait(fd, &tl, &point, 1, INT64_MAX, 0, NULL);
	dt = now_ms() - t0;
	check(r == 0 && dt >= 100, "TIMELINE_WAIT for point 2 returns once "
	    "point 1 has signalled too (the chain)");
	(void) printf("    point 2 after %" PRId64 " ms\n", dt);
	point = 0;
	check(drmSyncobjQuery(fd, &tl, &point, 1) == 0 && point == 2,
	    "QUERY is 2");

	(void) drmSyncobjDestroy(fd, so);
	(void) drmSyncobjDestroy(fd, tl);

	/*
	 * The soft interrupt dropped last references: the core parked
	 * their releases for its task queue.
	 */
	(void) usleep(200000);
	check(sync_counter("deferred") > deferred0, "the core parked "
	    "releases from interrupt context (kstat drm:0:sync deferred)");
	(void) printf("    deferred %" PRIu64 " -> %" PRIu64 "\n", deferred0,
	    sync_counter("deferred"));
}

static int
walk(int fd, uint32_t flags, uint32_t stop_at, struct cdrmnull_handles *h)
{
	(void) memset(h, 0, sizeof (*h));
	h->flags = flags;
	h->stop_at = stop_at;
	return (xioctl(fd, DRM_IOCTL_CDRMNULL_HANDLES, h));
}

static uint32_t
refs(int fd, uint32_t handle, uint32_t *kref_ok)
{
	struct cdrmnull_handles h;

	(void) memset(&h, 0, sizeof (h));
	h.refs_handle = handle;
	if (xioctl(fd, DRM_IOCTL_CDRMNULL_HANDLES, &h) != 0)
		return (UINT32_MAX);
	if (kref_ok != NULL)
		*kref_ok = h.kref_ok;
	return (h.refs);
}

/* Waits up to a second for the object's count to become `want`. */
static uint32_t
refs_settle(int fd, uint32_t handle, uint32_t want)
{
	uint32_t r = 0;
	int i;

	for (i = 0; i < 100; i++) {
		if ((r = refs(fd, handle, NULL)) == want)
			break;
		(void) usleep(10000);
	}
	return (r);
}

struct churn {
	int fd;
	volatile int stop;
	int errors;
	uint32_t created;
};

static void *
churn_thread(void *arg)
{
	struct churn *c = arg;
	uint32_t h;

	while (!c->stop) {
		if ((h = dumb(c->fd, NULL)) == 0) {
			c->errors++;
			break;
		}
		c->created++;
		if (gem_close(c->fd, h) != 0)
			c->errors++;
	}
	return (NULL);
}

static void
test_handles(int fd)
{
	uint32_t hs[16], i, kok = 0;
	uint64_t sum = 0, bytes = 0, sz = 0;
	struct cdrmnull_handles h;
	struct drm_gem_flink fl;
	struct drm_mode_map_dumb md;
	struct churn c;
	pthread_t th;
	int bad = 0, n = 0, dmabuf = -1;
	void *map;

	check(walk(fd, 0, 0, &h) == 0 && h.count == 0 && h.ret == 0,
	    "HANDLES on a file without handles: none");
	for (i = 0; i < 16; i++) {
		hs[i] = dumb(fd, &sz);
		sum += hs[i];
		bytes += sz;
	}
	check(walk(fd, CDRMNULL_HANDLES_LOOKUP, 0, &h) == 0 && h.count == 16 &&
	    h.handle_sum == sum && h.bytes == bytes && h.lookups == 16 &&
	    h.ret == 0, "HANDLES walks all 16 handles, each looked up from "
	    "inside the walk");
	check(walk(fd, CDRMNULL_HANDLES_STOP, 5, &h) == 0 && h.count == 5 &&
	    h.ret == 1, "HANDLES stops when the callback says so (ret 1)");
	check(walk(fd, 0x80, 0, &h) == EINVAL, "HANDLES with an unknown flag "
	    "is EINVAL");

	/* The object's reference count, as the driver reads it. */
	check(refs(fd, hs[0], &kok) == 1 && kok == 1, "a buffer with one "
	    "handle: refcount 1; kref_get_unless_zero/put round trip");
	(void) memset(&fl, 0, sizeof (fl));
	fl.handle = hs[0];
	check(xioctl(fd, DRM_IOCTL_GEM_FLINK, &fl) == 0 &&
	    refs(fd, hs[0], NULL) == 2, "GEM_FLINK: the name counts (2)");
	check(drmPrimeHandleToFD(fd, hs[0], DRM_CLOEXEC, &dmabuf) == 0 &&
	    refs(fd, hs[0], NULL) == 3, "PRIME export: the dma-buf counts (3)");
	(void) memset(&md, 0, sizeof (md));
	md.handle = hs[0];
	map = MAP_FAILED;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
		map = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, (off_t)md.offset);
	check(map != MAP_FAILED && refs(fd, hs[0], NULL) == 4,
	    "a user mapping counts (4)");
	if (map != MAP_FAILED)
		(void) munmap(map, (size_t)sz);
	check(refs_settle(fd, hs[0], 3) == 3, "munmap: back to 3");
	(void) close(dmabuf);
	check(refs_settle(fd, hs[0], 2) == 2, "closing the dma-buf: back to "
	    "2");

	for (i = 0; i < 16; i++)
		(void) gem_close(fd, hs[i]);
	check(walk(fd, 0, 0, &h) == 0 && h.count == 0, "after GEM_CLOSE of "
	    "all: no handle left");

	/* Walks while another thread creates and closes handles. */
	for (i = 0; i < 8; i++)
		hs[i] = dumb(fd, NULL);
	(void) memset(&c, 0, sizeof (c));
	c.fd = fd;
	if (pthread_create(&th, NULL, churn_thread, &c) != 0) {
		check(0, "start the churn thread");
		return;
	}
	for (n = 0; n < 3000; n++) {
		if (walk(fd, CDRMNULL_HANDLES_LOOKUP, 0, &h) != 0 ||
		    h.count < 8 || h.count > 9 || h.lookups != h.count ||
		    h.ret != 0)
			bad++;
	}
	c.stop = 1;
	(void) pthread_join(th, NULL);
	(void) printf("    %d walks, %u handles created and closed meanwhile, "
	    "%d bad walks, %d thread errors\n", n, c.created, bad, c.errors);
	check(bad == 0 && c.errors == 0 && c.created > 0, "walks stay exact "
	    "while another thread creates and closes handles");
	for (i = 0; i < 8; i++)
		(void) gem_close(fd, hs[i]);
	check(walk(fd, 0, 0, &h) == 0 && h.count == 0, "no handle left");
}

static void
test_print(int fd, uint64_t token)
{
	struct cdrmnull_print_selftest p;
	char want[64];

	(void) memset(&p, 0, sizeof (p));
	p.token = token;
	check(xioctl(fd, DRM_IOCTL_CDRMNULL_PRINT_SELFTEST, &p) == 0,
	    "PRINT_SELFTEST runs");
	(void) snprintf(want, sizeof (want), "%llx 42 abc -7",
	    (unsigned long long)token);
	check(strcmp(p.buf, want) == 0 && p.len == strlen(want),
	    "drm_printf/drm_puts through a driver's printer");
	(void) printf("    printer: \"%s\" (%u)\n", p.buf, p.len);
	p.flags = 1;
	check(xioctl(fd, DRM_IOCTL_CDRMNULL_PRINT_SELFTEST, &p) == EINVAL,
	    "PRINT_SELFTEST with flags is EINVAL");
}

int
main(int argc, char **argv)
{
	const char *card = getenv("DRM_CARD");
	uint64_t token;
	int fd;

	if (card == NULL)
		card = "/dev/dri/card0";
	token = argc > 1 ? strtoull(argv[1], NULL, 16) :
	    (uint64_t)time(NULL);
	if ((fd = open(card, O_RDWR)) < 0) {
		perror(card);
		return (2);
	}
	(void) printf("drm_r5b_test on %s, token %llx\n", card,
	    (unsigned long long)token);
	/* "print" after the token: the printing test alone. */
	if (argc <= 2 || strcmp(argv[2], "print") != 0) {
		test_ops_selftest(fd);
		test_core_structures(fd);
		test_handles(fd);
	}
	test_print(fd, token);
	(void) close(fd);
	(void) printf("drm_r5b_test: %d passed, %d failed\n", passes,
	    failures);
	return (failures == 0 ? 0 : 1);
}
