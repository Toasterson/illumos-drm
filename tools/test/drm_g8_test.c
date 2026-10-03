/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The G8 test of illumos-drm: importing (PRIME_FD_TO_HANDLE) or opening
 * (GEM_OPEN, MODE_GETFB) an object that a file holds already must leave
 * that file's existing handle working. The core calls a driver's GEM
 * open hook when a file gets its first handle to an object and its close
 * hook when the file's last handle to it goes; virtio_gpu attaches the
 * resource to the file's virgl context there and detaches it. Before G8,
 * a second import ran the open hook and then undid it with the close hook,
 * which detached the resource the first handle still used: Xorg's DRI3
 * PixmapFromBuffer of its own front buffer left the screen black (G3, G4).
 *
 * For each device given, on dumb buffers:
 *   - a file imports its own buffer twice: the same handle both times, and
 *     the gem kstat's handles, attached, objects, umem_bytes and
 *     dma_segments do not change; the buffer's mapping keeps its contents;
 *   - a second file imports it twice: one handle, attached + 1;
 *   - GEM_OPEN of the buffer's name in the first file gives a second
 *     handle (as in Linux) with no new attachment, and closing it leaves the
 *     attachment and the first handle alone; MODE_GETFB likewise on a
 *     device with mode setting;
 *   - each other device imports the buffer twice (cross-driver): one
 *     handle, one import object, attached + 1;
 *   - everything goes back to the starting counts.
 * On virtio_gpu with virgl, the DRI3 case on a 3D texture the size of the
 * display: the file that made it (the X server) imports it twice, another
 * file (a client) imports it twice too; then, through the first file's
 * original handle, a virgl clear reads back with TRANSFER_FROM_HOST,
 * TRANSFER_TO_HOST of a pattern written to the mapping reads back, and the
 * texture is put on the display (SETCRTC) and flushed (DIRTYFB:
 * RESOURCE_FLUSH). The client draws into the shared texture through its
 * own context and closes its handle; the first file's context still has
 * the texture. With G8_HOLD=<seconds> the test waits that long with the
 * texture on the display (for a host screendump).
 *
 * Run as root:
 *	DRM_CARD_C=/dev/dri/cardN DRM_CARD_R=/dev/dri/cardM \
 *	    [DRM_CARD_V=/dev/dri/cardK DRM_RENDER_V=/dev/dri/renderDL] ./drm_g8_test
 * Any of the three may be left out (at least one is needed). Build:
 *	gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_g8_test drm_g8_test.c \
 *	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat
 * where drm/ holds the headers of tools/uapi/generate.sh (libdrm 2.4.109,
 * patched, with virtgpu_drm.h of Linux v6.12).
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <kstat.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm.h"
#include "drm_mode.h"
#include "drm_fourcc.h"
#include "virtgpu_drm.h"

static int failures, passes;

static void
check(int ok, const char *fmt, ...)
{
	va_list ap;

	(void) printf("%s ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	(void) vprintf(fmt, ap);
	va_end(ap);
	(void) printf("\n");
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

struct dev {
	const char *name;	/* driver */
	const char *card;
	const char *render;	/* optional */
	int fd;			/* the first file (card node) */
};

/* --- the gem kstat ------------------------------------------------------- */

struct gem {
	int ok;
	uint64_t objects, umem_bytes, dma_segments, handles, attached;
};

static struct gem
gem_stats(const char *drv)
{
	static const char *const names[] = {
		"objects", "umem_bytes", "dma_segments", "handles", "attached"
	};
	struct gem g;
	uint64_t *out[] = { &g.objects, &g.umem_bytes, &g.dma_segments, &g.handles, &g.attached };
	kstat_ctl_t *kc;
	kstat_t *ks;

	(void) memset(&g, 0, sizeof (g));
	if ((kc = kstat_open()) == NULL)
		return (g);
	if ((ks = kstat_lookup(kc, (char *)drv, -1, "gem")) != NULL &&
	    kstat_read(kc, ks, NULL) != -1) {
		g.ok = 1;
		for (int i = 0; i < 5; i++) {
			kstat_named_t *kn = kstat_data_lookup(ks, (char *)names[i]);

			if (kn == NULL) {
				g.ok = 0;
				(void) printf("INFO %s:gem has no %s\n", drv, names[i]);
			} else {
				*out[i] = kn->value.ui64;
			}
		}
	}
	(void) kstat_close(kc);
	return (g);
}

static void
print_gem(const char *drv, const char *when, struct gem *g)
{
	(void) printf("INFO %s gem %s: objects %llu umem_bytes %llu dma_segments %llu handles %llu "
	    "attached %llu\n", drv, when, (unsigned long long)g->objects,
	    (unsigned long long)g->umem_bytes, (unsigned long long)g->dma_segments,
	    (unsigned long long)g->handles, (unsigned long long)g->attached);
}

/* Whether the counters of `b` differ from `a` by exactly the deltas. */
static int
gem_delta(struct gem *a, struct gem *b, int64_t objects, int64_t handles, int64_t attached)
{
	return (a->ok && b->ok && b->objects == a->objects + objects &&
	    b->handles == a->handles + handles && b->attached == a->attached + attached);
}

/* --- GEM helpers ---------------------------------------------------------- */

static uint32_t
dumb(int fd, uint64_t *size)
{
	struct drm_mode_create_dumb c;

	(void) memset(&c, 0, sizeof (c));
	c.width = 128;
	c.height = 64;
	c.bpp = 32;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0)
		return (0);
	*size = c.size;
	return (c.handle);
}

static uint32_t *
map_dumb(int fd, uint32_t h, size_t size)
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

static int
gem_close(int fd, uint32_t h)
{
	struct drm_gem_close c;

	(void) memset(&c, 0, sizeof (c));
	c.handle = h;
	return (xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c));
}

static int
export(int fd, uint32_t h)
{
	struct drm_prime_handle ph;

	(void) memset(&ph, 0, sizeof (ph));
	ph.handle = h;
	ph.flags = DRM_CLOEXEC | DRM_RDWR;
	ph.fd = -1;
	if (xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) != 0)
		return (-1);
	return (ph.fd);
}

static uint32_t
import(int fd, int dfd)
{
	struct drm_prime_handle ph;

	(void) memset(&ph, 0, sizeof (ph));
	ph.fd = dfd;
	if (xioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0)
		return (0);
	return (ph.handle);
}

static int
pattern_ok(const uint32_t *p, size_t n, uint32_t tag)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != (tag | (uint32_t)(i & 0xffff)))
			return (0);
	}
	return (1);
}

static void
pattern(uint32_t *p, size_t n, uint32_t tag)
{
	for (size_t i = 0; i < n; i++)
		p[i] = tag | (uint32_t)(i & 0xffff);
}

/*
 * The same-device and cross-driver re-imports of a dumb buffer of `d`
 * (see the top of the file); `all` holds every device under test.
 */
static void
reimport(struct dev *d, struct dev *all, int n)
{
	struct gem g0, g1, g2;
	uint64_t size;
	uint32_t h, h2, hb, hb2, name, *p;
	struct drm_gem_flink fl;
	struct drm_gem_open op;
	int dfd, f2;
	size_t words;

	(void) printf("--- %s: re-import of a buffer the file holds\n", d->name);
	g0 = gem_stats(d->name);
	print_gem(d->name, "at the start", &g0);
	check(g0.ok, "%s: the gem kstat has objects, umem_bytes, dma_segments, handles, attached",
	    d->name);
	h = dumb(d->fd, &size);
	check(h != 0, "%s: a 128x64 dumb buffer", d->name);
	if (h == 0)
		return;
	words = (size_t)size / 4;
	p = map_dumb(d->fd, h, size);
	check(p != NULL, "%s: map it", d->name);
	if (p == NULL) {
		(void) gem_close(d->fd, h);
		return;
	}
	pattern(p, words, 0x67000000u);
	g1 = gem_stats(d->name);
	check(gem_delta(&g0, &g1, 1, 1, 1), "%s: one object, one handle, one attachment", d->name);
	dfd = export(d->fd, h);
	check(dfd >= 0, "%s: export it (PRIME_HANDLE_TO_FD)", d->name);
	if (dfd < 0) {
		(void) munmap((void *)p, size);
		(void) gem_close(d->fd, h);
		return;
	}

	/* The exporting file imports its own buffer, twice. */
	check(import(d->fd, dfd) == h, "%s: the exporting file imports it: its own handle %u", d->name, h);
	check(import(d->fd, dfd) == h, "%s: and again: the same handle", d->name);
	g2 = gem_stats(d->name);
	print_gem(d->name, "after two self-imports", &g2);
	check(gem_delta(&g1, &g2, 0, 0, 0) && g2.umem_bytes == g1.umem_bytes &&
	    g2.dma_segments == g1.dma_segments,
	    "%s: kstat: no handle, attachment, object, memory or segment added or taken away",
	    d->name);
	check(pattern_ok(p, words, 0x67000000u), "%s: the mapping still shows the buffer", d->name);

	/* A second file imports it twice. */
	f2 = open(d->card, O_RDWR);
	check(f2 >= 0, "%s: a second file", d->name);
	if (f2 >= 0) {
		hb = import(f2, dfd);
		hb2 = import(f2, dfd);
		check(hb != 0 && hb2 == hb, "%s: the second file imports it twice: one handle (%u, %u)",
		    d->name, hb, hb2);
		g2 = gem_stats(d->name);
		check(gem_delta(&g1, &g2, 0, 1, 1), "%s: kstat: one more handle and attachment", d->name);
		(void) close(f2);
		g2 = gem_stats(d->name);
		check(gem_delta(&g1, &g2, 0, 0, 0), "%s: closing the second file takes them away",
		    d->name);
	}

	/* GEM_OPEN in the creating file: a second handle, no attachment. */
	(void) memset(&fl, 0, sizeof (fl));
	fl.handle = h;
	check(xioctl(d->fd, DRM_IOCTL_GEM_FLINK, &fl) == 0 && fl.name != 0, "%s: GEM_FLINK", d->name);
	name = fl.name;
	(void) memset(&op, 0, sizeof (op));
	op.name = name;
	h2 = 0;
	if (xioctl(d->fd, DRM_IOCTL_GEM_OPEN, &op) == 0)
		h2 = op.handle;
	check(h2 != 0 && h2 != h, "%s: GEM_OPEN in the same file gives a second handle %u (as Linux)",
	    d->name, h2);
	g2 = gem_stats(d->name);
	check(gem_delta(&g1, &g2, 0, 1, 0), "%s: kstat: one more handle, no new attachment", d->name);
	hb = import(d->fd, dfd);
	check(hb == h || hb == h2, "%s: an import with two handles finds one of them (%u)", d->name,
	    hb);
	check(h2 != 0 && gem_close(d->fd, h2) == 0, "%s: GEM_CLOSE of the second handle", d->name);
	g2 = gem_stats(d->name);
	check(gem_delta(&g1, &g2, 0, 0, 0), "%s: kstat: the attachment stays with the first handle",
	    d->name);

	/* MODE_GETFB, on devices with mode setting. */
	{
		uint32_t fb = 0, hg = 0;
		int e = drmModeAddFB(d->fd, 128, 64, 24, 32, 128 * 4, h, &fb);

		if (e != 0) {
			(void) printf("INFO %s: ADDFB: %s; no MODE_GETFB check\n", d->name,
			    strerror(-e));
		} else {
			drmModeFBPtr f = drmModeGetFB(d->fd, fb);

			if (f != NULL) {
				hg = f->handle;
				drmModeFreeFB(f);
			}
			check(hg != 0 && hg != h, "%s: MODE_GETFB gives a new handle %u (as Linux)",
			    d->name, hg);
			g2 = gem_stats(d->name);
			check(gem_delta(&g1, &g2, 0, 1, 0), "%s: kstat: no new attachment", d->name);
			check(hg != 0 && gem_close(d->fd, hg) == 0, "%s: close the MODE_GETFB handle",
			    d->name);
			check(drmModeRmFB(d->fd, fb) == 0, "%s: RMFB", d->name);
			g2 = gem_stats(d->name);
			check(gem_delta(&g1, &g2, 0, 0, 0), "%s: kstat: the attachment stays", d->name);
		}
	}

	/* Every other device imports it twice. */
	for (int i = 0; i < n; i++) {
		struct dev *o = &all[i];
		struct gem o1, o2;
		uint32_t ho, ho2, *q;

		if (o == d)
			continue;
		o1 = gem_stats(o->name);
		ho = import(o->fd, dfd);
		ho2 = import(o->fd, dfd);
		check(ho != 0 && ho2 == ho, "%s imports %s's buffer twice: one handle (%u, %u)",
		    o->name, d->name, ho, ho2);
		o2 = gem_stats(o->name);
		check(gem_delta(&o1, &o2, 1, 1, 1),
		    "%s: kstat: one import object, one handle, one attachment", o->name);
		q = ho != 0 ? map_dumb(o->fd, ho, size) : NULL;
		check(q != NULL && pattern_ok(q, words, 0x67000000u),
		    "%s maps the import and sees %s's buffer", o->name, d->name);
		if (q != NULL)
			(void) munmap((void *)q, size);
		check(ho != 0 && gem_close(o->fd, ho) == 0, "%s: close the import", o->name);
		o2 = gem_stats(o->name);
		check(gem_delta(&o1, &o2, 0, 0, 0), "%s: kstat: back to the start", o->name);
		g2 = gem_stats(d->name);
		check(gem_delta(&g1, &g2, 0, 0, 0), "%s: kstat: unchanged by %s's import", d->name,
		    o->name);
	}

	/* The first handle still works: write through a new mapping. */
	{
		uint32_t *p2 = map_dumb(d->fd, h, size);

		if (p2 != NULL) {
			pattern(p2, words, 0x76000000u);
			(void) munmap((void *)p2, size);
		}
		check(p2 != NULL && pattern_ok(p, words, 0x76000000u),
		    "%s: the first handle maps again and both mappings agree", d->name);
	}
	(void) munmap((void *)p, size);
	(void) close(dfd);
	check(gem_close(d->fd, h) == 0, "%s: close the buffer", d->name);
	g2 = gem_stats(d->name);
	print_gem(d->name, "at the end", &g2);
	check(gem_delta(&g0, &g2, 0, 0, 0), "%s: kstat: back to the start", d->name);
}

/* --- virtio_gpu with virgl: the DRI3 case ---------------------------------- */

/* virglrenderer's virgl_protocol.h and virgl_hw.h. */
#define	VIRGL_CMD0(cmd, obj, len)	((cmd) | ((obj) << 8) | ((len) << 16))
#define	VIRGL_CCMD_CREATE_OBJECT	1
#define	VIRGL_CCMD_DESTROY_OBJECT	3
#define	VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define	VIRGL_CCMD_CLEAR		7
#define	VIRGL_OBJECT_SURFACE		8
#define	VIRGL_FORMAT_B8G8R8X8_UNORM	2
#define	VIRGL_BIND_RENDER_TARGET	(1u << 1)
#define	VIRGL_BIND_SAMPLER_VIEW		(1u << 3)
#define	VIRGL_BIND_SCANOUT		(1u << 18)
#define	PIPE_TEXTURE_2D			2
#define	PIPE_CLEAR_COLOR0		(1 << 2)

static uint32_t
fbits(float x)
{
	uint32_t u;

	(void) memcpy(&u, &x, 4);
	return (u);
}

/* Clears resource `res` to (r, g, b) through `f`'s context; 0 or errno. */
static int
clear(int f, uint32_t bo, uint32_t res, uint32_t surf, float r, float g, float b)
{
	uint32_t s[24];
	size_t n = 0;
	struct drm_virtgpu_execbuffer x;
	struct drm_virtgpu_3d_wait w;
	int e;

	s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
	s[n++] = surf;
	s[n++] = res;
	s[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
	s[n++] = 0;
	s[n++] = 0;
	s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
	s[n++] = 1;
	s[n++] = 0;
	s[n++] = surf;
	s[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
	s[n++] = PIPE_CLEAR_COLOR0;
	s[n++] = fbits(r);
	s[n++] = fbits(g);
	s[n++] = fbits(b);
	s[n++] = fbits(1.0f);
	s[n++] = 0;
	s[n++] = 0;
	s[n++] = 0;
	s[n++] = VIRGL_CMD0(VIRGL_CCMD_DESTROY_OBJECT, VIRGL_OBJECT_SURFACE, 1);
	s[n++] = surf;
	(void) memset(&x, 0, sizeof (x));
	x.size = (uint32_t)(n * 4);
	x.command = (uintptr_t)s;
	x.bo_handles = (uintptr_t)&bo;
	x.num_bo_handles = 1;
	x.fence_fd = -1;
	x.syncobj_stride = sizeof (struct drm_virtgpu_execbuffer_syncobj);
	if ((e = xioctl(f, DRM_IOCTL_VIRTGPU_EXECBUFFER, &x)) != 0)
		return (e);
	(void) memset(&w, 0, sizeof (w));
	w.handle = bo;
	return (xioctl(f, DRM_IOCTL_VIRTGPU_WAIT, &w));
}

/* TRANSFER_TO_HOST (to_host) or _FROM_HOST of the whole texture; waits. */
static int
transfer(int f, uint32_t bo, uint32_t w, uint32_t h, int to_host)
{
	struct drm_virtgpu_3d_transfer_to_host t;
	struct drm_virtgpu_3d_wait wt;
	int e;

	(void) memset(&t, 0, sizeof (t));
	t.bo_handle = bo;
	t.box.w = w;
	t.box.h = h;
	t.box.d = 1;
	t.stride = w * 4;
	e = xioctl(f, to_host ? DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST :
	    DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &t);
	if (e != 0)
		return (e);
	(void) memset(&wt, 0, sizeof (wt));
	wt.handle = bo;
	return (xioctl(f, DRM_IOCTL_VIRTGPU_WAIT, &wt));
}

/* The number of pixels of B8G8R8X8 `p` that are not about (b, g, r). */
static size_t
off_colour(const uint8_t *p, size_t pixels, int b, int g, int r)
{
	size_t bad = 0;

	for (size_t i = 0; i < pixels; i++, p += 4) {
		if (abs(p[0] - b) > 2 || abs(p[1] - g) > 2 || abs(p[2] - r) > 2)
			bad++;
	}
	return (bad);
}

/* Reads the texture back through `f` and checks that it is (b, g, r). */
static void
expect_colour(int f, uint32_t bo, uint8_t *map, uint32_t w, uint32_t h, int b, int g, int r,
    const char *what)
{
	size_t bad;
	int e;

	(void) memset(map, 0x11, (size_t)w * h * 4);
	e = transfer(f, bo, w, h, 0);
	bad = off_colour(map, (size_t)w * h, b, g, r);
	check(e == 0 && bad == 0, "%s (TRANSFER_FROM_HOST: %s, %zu of %u pixels differ, "
	    "first %02x %02x %02x)", what, e == 0 ? "ok" : strerror(e), bad, w * h,
	    map[0], map[1], map[2]);
}

static int
connector_mode(int fd, uint32_t *conn, uint32_t *crtc, drmModeModeInfo *mode)
{
	drmModeResPtr res = drmModeGetResources(fd);
	int found = 0;

	if (res == NULL)
		return (0);
	for (int i = 0; i < res->count_connectors && !found; i++) {
		drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);

		if (c == NULL)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
		    res->count_crtcs > 0) {
			*conn = c->connector_id;
			*crtc = res->crtcs[0];
			*mode = c->modes[0];
			for (int m = 0; m < c->count_modes; m++) {
				if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
					*mode = c->modes[m];
					break;
				}
			}
			found = 1;
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return (found);
}

static void
dri3(struct dev *d)
{
	struct drm_virtgpu_resource_create rc;
	struct drm_virtgpu_map m;
	struct gem g0, g1, g2;
	drmModeModeInfo mode;
	drmModeCrtcPtr saved = NULL;
	uint32_t conn = 0, crtc = 0, w = 1024, h = 768, bo, res, hx, hx2, hy, hy2, fb = 0;
	uint8_t *map;
	size_t size;
	int y, dfd, have_mode, e;
	uint64_t v = 0;
	struct drm_virtgpu_getparam gp;

	(void) printf("--- %s: the DRI3 case (virgl)\n", d->name);
	(void) memset(&gp, 0, sizeof (gp));
	gp.param = VIRTGPU_PARAM_3D_FEATURES;
	gp.value = (uintptr_t)&v;
	if (xioctl(d->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) != 0 || v == 0) {
		(void) printf("INFO %s: no virgl; no DRI3 check\n", d->name);
		return;
	}
	have_mode = connector_mode(d->fd, &conn, &crtc, &mode);
	if (have_mode) {
		w = mode.hdisplay;
		h = mode.vdisplay;
		(void) printf("INFO %s: connector %u, crtc %u, mode %s (%ux%u)\n", d->name, conn,
		    crtc, mode.name, w, h);
	}
	check(have_mode, "%s: a connected connector with a mode", d->name);
	g0 = gem_stats(d->name);

	/* The server's front buffer: a 3D texture of the display's size. */
	(void) memset(&rc, 0, sizeof (rc));
	rc.target = PIPE_TEXTURE_2D;
	rc.format = VIRGL_FORMAT_B8G8R8X8_UNORM;
	rc.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_SCANOUT;
	rc.width = w;
	rc.height = h;
	rc.depth = 1;
	rc.array_size = 1;
	rc.size = w * h * 4;
	rc.stride = w * 4;
	e = xioctl(d->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc);
	check(e == 0, "%s: RESOURCE_CREATE of a %ux%u texture", d->name, w, h);
	if (e != 0)
		return;
	bo = rc.bo_handle;
	res = rc.res_handle;
	size = (size_t)w * h * 4;
	(void) memset(&m, 0, sizeof (m));
	m.handle = bo;
	map = xioctl(d->fd, DRM_IOCTL_VIRTGPU_MAP, &m) == 0 ?
	    mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)m.offset) : MAP_FAILED;
	check(map != MAP_FAILED, "%s: map the texture", d->name);
	if (map == MAP_FAILED) {
		(void) gem_close(d->fd, bo);
		return;
	}
	check(clear(d->fd, bo, res, 1, 1, 0, 0) == 0, "%s: the server clears it red", d->name);
	expect_colour(d->fd, bo, map, w, h, 0, 0, 0xff, "the texture reads back red");
	g1 = gem_stats(d->name);

	/* DRI3: the server imports its own buffer, twice. */
	dfd = export(d->fd, bo);
	check(dfd >= 0, "%s: export the texture (BufferFromPixmap)", d->name);
	if (dfd < 0)
		goto out;
	hx = import(d->fd, dfd);
	hx2 = import(d->fd, dfd);
	check(hx == bo && hx2 == bo, "%s: the server imports it twice (PixmapFromBuffer): its own "
	    "handle %u (%u, %u)", d->name, bo, hx, hx2);
	/* A client imports it twice too. */
	y = d->render != NULL ? open(d->render, O_RDWR) : open(d->card, O_RDWR);
	hy = y >= 0 ? import(y, dfd) : 0;
	hy2 = y >= 0 ? import(y, dfd) : 0;
	check(hy != 0 && hy2 == hy, "%s: a client imports it twice: one handle (%u, %u)", d->name,
	    hy, hy2);
	g2 = gem_stats(d->name);
	print_gem(d->name, "after the imports", &g2);
	check(gem_delta(&g1, &g2, 0, 1, 1), "%s: kstat: only the client's handle and attachment "
	    "were added", d->name);

	/* The server's context still has the texture. */
	check(clear(d->fd, bo, res, 2, 0, 1, 0) == 0, "%s: the server clears it green through its "
	    "first handle", d->name);
	expect_colour(d->fd, bo, map, w, h, 0, 0xff, 0, "the server reads back green after the "
	    "re-imports");
	(void) memset(map, 0, size);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		map[4 * i] = 0xff;		/* blue */
		map[4 * i + 1] = (uint8_t)(i & 0x3f);
	}
	e = transfer(d->fd, bo, w, h, 1);
	check(e == 0, "%s: TRANSFER_TO_HOST through the first handle: %s", d->name,
	    e == 0 ? "ok" : strerror(e));
	(void) memset(map, 0x11, size);
	e = transfer(d->fd, bo, w, h, 0);
	{
		size_t bad = 0;

		for (size_t i = 0; i < (size_t)w * h; i++) {
			if (map[4 * i] != 0xff || map[4 * i + 1] != (uint8_t)(i & 0x3f) ||
			    map[4 * i + 2] != 0)
				bad++;
		}
		check(e == 0 && bad == 0, "%s: what TRANSFER_TO_HOST sent reads back (%zu of %u "
		    "pixels differ)", d->name, bad, w * h);
	}

	/* Show it (SETCRTC) and flush it (DIRTYFB: RESOURCE_FLUSH). */
	if (have_mode) {
		uint32_t handles[4] = { bo }, pitches[4] = { w * 4 }, offsets[4] = { 0 };

		(void) drmSetMaster(d->fd);
		saved = drmModeGetCrtc(d->fd, crtc);
		e = drmModeAddFB2(d->fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
		    &fb, 0);
		check(e == 0, "%s: ADDFB2 of the texture", d->name);
		if (e == 0) {
			check(clear(d->fd, bo, res, 3, 0, 0.5f, 1) == 0, "%s: the server clears it "
			    "azure (0, 128, 255)", d->name);
			e = drmModeSetCrtc(d->fd, crtc, fb, 0, 0, &conn, 1, &mode);
			check(e == 0, "%s: SETCRTC shows it: %s", d->name, strerror(-e));
			e = drmModeDirtyFB(d->fd, fb, NULL, 0);
			check(e == 0, "%s: DIRTYFB (RESOURCE_FLUSH): %s", d->name, strerror(-e));
			expect_colour(d->fd, bo, map, w, h, 0xff, 0x80, 0, "the shown texture reads "
			    "back azure");
			if (getenv("G8_HOLD") != NULL) {
				(void) printf("INFO holding the display for %s s\n",
				    getenv("G8_HOLD"));
				(void) fflush(stdout);
				(void) sleep((unsigned)atoi(getenv("G8_HOLD")));
			}
		}
	}

	/* The client draws into it through its own context, then lets go. */
	if (hy != 0) {
		check(clear(y, hy, res, 4, 1, 0, 1) == 0, "%s: the client clears it magenta",
		    d->name);
		expect_colour(d->fd, bo, map, w, h, 0xff, 0, 0xff, "the server reads back the "
		    "client's magenta");
		check(gem_close(y, hy) == 0, "%s: the client closes its handle", d->name);
		check(gem_close(y, hy) == EINVAL, "%s: it had only one", d->name);
	}
	if (y >= 0)
		(void) close(y);
	check(clear(d->fd, bo, res, 5, 1, 1, 0) == 0, "%s: the server clears it yellow after the "
	    "client let go", d->name);
	expect_colour(d->fd, bo, map, w, h, 0, 0xff, 0xff, "the server reads back yellow");
	if (fb != 0) {
		check(drmModeDirtyFB(d->fd, fb, NULL, 0) == 0, "%s: DIRTYFB again", d->name);
		if (saved != NULL && saved->buffer_id != 0 && saved->mode_valid)
			(void) drmModeSetCrtc(d->fd, crtc, saved->buffer_id, saved->x, saved->y,
			    &conn, 1, &saved->mode);
		else
			(void) drmModeSetCrtc(d->fd, crtc, 0, 0, 0, NULL, 0, NULL);
		check(drmModeRmFB(d->fd, fb) == 0, "%s: RMFB", d->name);
	}
	if (saved != NULL)
		drmModeFreeCrtc(saved);
	(void) close(dfd);
out:
	(void) munmap((void *)map, size);
	check(gem_close(d->fd, bo) == 0, "%s: the server closes the texture", d->name);
	g2 = gem_stats(d->name);
	check(gem_delta(&g0, &g2, 0, 0, 0), "%s: kstat: back to the start", d->name);
}

int
main(void)
{
	struct dev d[3];
	int nd = 0;
	const char *s;

	(void) memset(d, 0, sizeof (d));
	if ((s = getenv("DRM_CARD_C")) != NULL && *s != '\0') {
		d[nd].name = "cdrmnull";
		d[nd++].card = s;
	}
	if ((s = getenv("DRM_CARD_R")) != NULL && *s != '\0') {
		d[nd].name = "rdrmnull";
		d[nd++].card = s;
	}
	if ((s = getenv("DRM_CARD_V")) != NULL && *s != '\0') {
		d[nd].name = "virtio_gpu";
		d[nd].render = getenv("DRM_RENDER_V");
		d[nd++].card = s;
	}
	if (nd == 0) {
		(void) fprintf(stderr, "set DRM_CARD_C, DRM_CARD_R and/or DRM_CARD_V\n");
		return (2);
	}
	for (int i = 0; i < nd; i++) {
		d[i].fd = open(d[i].card, O_RDWR);
		(void) printf("INFO %s is %s\n", d[i].name, d[i].card);
		check(d[i].fd >= 0, "%s: open the card node", d[i].name);
		if (d[i].fd < 0)
			return (1);
	}
	for (int i = 0; i < nd; i++)
		reimport(&d[i], d, nd);
	for (int i = 0; i < nd; i++) {
		if (strcmp(d[i].name, "virtio_gpu") == 0)
			dri3(&d[i]);
	}
	for (int i = 0; i < nd; i++)
		(void) close(d[i].fd);
	(void) printf("drm_g8_test: %d passed, %d failed\n", passes, failures);
	return (failures != 0);
}
