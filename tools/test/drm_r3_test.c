/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Userland test of the illumos-drm R3 core (mode objects, properties,
 * blobs, framebuffers, atomic and legacy mode setting, vblanks and events
 * through read(2) and poll(2)), run as root against rdrmnull.
 *
 * The mode-setting calls go through the installed libdrm 2.4.109
 * (xf86drmMode.h) where it has them and through raw ioctls where the
 * errno or the exact argument matters. The software scanout of rdrmnull
 * (RDRMNULL_IOCTL_SCANOUT) reports which framebuffer is on "screen" and a
 * checksum of its visible pixels, which the test compares with the
 * pattern it wrote. The kms and gem kstats (rdrmnull:0:kms, :gem) are read
 * with libkstat to check that every framebuffer, blob, event and object is
 * released.
 *
 * Every check prints PASS or FAIL; the exit status is the number of
 * failures (capped at 255 by the shell). INFO lines carry measurements.
 *
 * Build on illumos, with the patched libdrm headers in $INC:
 *   gcc -m64 -Wall -I$INC -I/usr/include/libdrm -o drm_r3_test drm_r3_test.c \
 *       -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread
 * and the 32-bit probe (no libdrm):
 *   gcc -m32 -Wall -DPROBE32 -I$INC -o drm_r3_probe32 drm_r3_test.c
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "drm.h"
#include "drm_mode.h"

#define	CARD	"/dev/dri/card0"
#define	RENDER	"/dev/dri/renderD128"

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

/* 32-bit processes get ENOTSUP for the mode-setting ioctls. */
int
main(void)
{
	struct drm_mode_card_res r;
	struct drm_mode_atomic a;
	int fd = open(CARD, O_RDWR);

	check(fd >= 0, "32-bit open " CARD);
	if (fd < 0)
		return (1);
	(void) memset(&r, 0, sizeof (r));
	errno = 0;
	check(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == -1 && errno == ENOTSUP,
	    "32-bit MODE_GETRESOURCES fails with ENOTSUP");
	(void) memset(&a, 0, sizeof (a));
	errno = 0;
	check(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &a) == -1 && errno == ENOTSUP,
	    "32-bit MODE_ATOMIC fails with ENOTSUP");
	(void) close(fd);
	(void) printf("probe32: %d failures\n", failures);
	return (failures);
}

#else	/* PROBE32 */

#include <kstat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define	FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define	XRGB8888	FOURCC('X', 'R', '2', '4')
#define	ARGB8888	FOURCC('A', 'R', '2', '4')
#define	RGB565		FOURCC('R', 'G', '1', '6')
#define	NV12		FOURCC('N', 'V', '1', '2')
#define	MOD_LINEAR	0ULL
#define	MOD_X_TILED	((1ULL << 56) | 1)

/* rdrmnull's scanout ioctl (examples/rdrmnull/src/kms.rs). */
struct rdrmnull_scanout {
	uint32_t fb_id, active, x, y, width, height, format, pad;
	uint64_t frames, latches, checksum;
};
_Static_assert(sizeof (struct rdrmnull_scanout) == 56, "layout");
#define	RDRMNULL_IOCTL_SCANOUT DRM_IOR(DRM_COMMAND_BASE + 0x03, struct rdrmnull_scanout)

/* --- kstats --- */

struct kms {
	uint64_t framebuffers, blobs, events_outstanding, events_delivered;
	uint64_t events_discarded, vblanks, flips, commits;
};

static int
kstat_get(const char *name, const char **fields, uint64_t *out, int n)
{
	kstat_ctl_t *kc;
	kstat_t *ksp;
	int ok = 0, i;

	if ((kc = kstat_open()) == NULL)
		return (0);
	ksp = kstat_lookup(kc, "rdrmnull", 0, (char *)name);
	if (ksp != NULL && kstat_read(kc, ksp, NULL) != -1) {
		ok = 1;
		for (i = 0; i < n; i++) {
			kstat_named_t *kn = kstat_data_lookup(ksp, (char *)fields[i]);
			if (kn == NULL)
				ok = 0;
			else
				out[i] = kn->value.ui64;
		}
	}
	(void) kstat_close(kc);
	return (ok);
}

static int
kms_stats(struct kms *k)
{
	static const char *f[] = { "framebuffers", "blobs", "events_outstanding",
	    "events_delivered", "events_discarded", "vblanks", "flips", "commits" };

	return (kstat_get("kms", f, (uint64_t *)k, 8));
}

static int
kms_idle(const char *when)
{
	struct kms k;
	int ok = kms_stats(&k);

	(void) printf("INFO kms kstat %s: framebuffers %llu blobs %llu events_outstanding %llu "
	    "delivered %llu discarded %llu vblanks %llu flips %llu commits %llu\n", when,
	    (unsigned long long)k.framebuffers, (unsigned long long)k.blobs,
	    (unsigned long long)k.events_outstanding, (unsigned long long)k.events_delivered,
	    (unsigned long long)k.events_discarded, (unsigned long long)k.vblanks,
	    (unsigned long long)k.flips, (unsigned long long)k.commits);
	/* Two IN_FORMATS blobs and the EDID blob belong to the device. */
	return (ok && k.framebuffers == 0 && k.blobs == 3 && k.events_outstanding == 0);
}

static int
gem_idle(void)
{
	static const char *f[] = { "objects", "handles", "names", "offsets", "mappings" };
	uint64_t v[5];

	return (kstat_get("gem", f, v, 5) && v[0] == 0 && v[1] == 0 && v[2] == 0 &&
	    v[3] == 0 && v[4] == 0);
}

/* --- helpers --- */

static int
xioctl(int fd, unsigned long req, void *arg)
{
	return (ioctl(fd, req, arg) == 0 ? 0 : errno);
}

static double
now_s(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

static void
msleep(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	(void) nanosleep(&ts, NULL);
}

struct buf {
	uint32_t handle, pitch, w, h, bpp;
	uint64_t size;
	uint8_t *map;
};

static int
buf_create(int fd, uint32_t w, uint32_t h, uint32_t bpp, struct buf *b)
{
	struct drm_mode_create_dumb c;
	struct drm_mode_map_dumb m;

	(void) memset(b, 0, sizeof (*b));
	(void) memset(&c, 0, sizeof (c));
	c.width = w;
	c.height = h;
	c.bpp = bpp;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0)
		return (0);
	b->handle = c.handle;
	b->pitch = c.pitch;
	b->size = c.size;
	b->w = w;
	b->h = h;
	b->bpp = bpp;
	(void) memset(&m, 0, sizeof (m));
	m.handle = c.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) != 0)
		return (0);
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	return (b->map != MAP_FAILED);
}

static void
buf_destroy(int fd, struct buf *b)
{
	struct drm_mode_destroy_dumb d;

	if (b->map != NULL && b->map != MAP_FAILED)
		(void) munmap((void *)b->map, b->size);
	(void) memset(&d, 0, sizeof (d));
	d.handle = b->handle;
	(void) xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	b->map = NULL;
}

/* Fills a 32-bit buffer with a pattern that depends on the seed. */
static void
buf_fill(struct buf *b, uint32_t seed)
{
	uint32_t x, y;

	for (y = 0; y < b->h; y++) {
		uint32_t *row = (uint32_t *)(b->map + (size_t)y * b->pitch);
		for (x = 0; x < b->w; x++)
			row[x] = (x * 2654435761u) ^ (y * 40503u) ^ seed;
	}
}

/* The checksum rdrmnull computes: FNV-1a over the visible rows. */
static uint64_t
fnv(const struct buf *b, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t cpp)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	uint32_t y, i;

	for (y = y0; y < y0 + h; y++) {
		const uint8_t *p = b->map + (size_t)y * b->pitch + (size_t)x0 * cpp;
		for (i = 0; i < w * cpp; i++) {
			hash ^= p[i];
			hash *= 0x100000001b3ULL;
		}
	}
	return (hash);
}

static int
addfb2(int fd, uint32_t w, uint32_t h, uint32_t fmt, uint32_t handle, uint32_t pitch,
    uint32_t flags, uint64_t mod, uint32_t *fb)
{
	struct drm_mode_fb_cmd2 f;
	int e;

	(void) memset(&f, 0, sizeof (f));
	f.width = w;
	f.height = h;
	f.pixel_format = fmt;
	f.flags = flags;
	f.handles[0] = handle;
	f.pitches[0] = pitch;
	f.modifier[0] = mod;
	e = xioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f);
	if (e == 0 && fb != NULL)
		*fb = f.fb_id;
	return (e);
}

static int
rmfb(int fd, uint32_t id)
{
	return (xioctl(fd, DRM_IOCTL_MODE_RMFB, &id));
}

static int
scanout(int fd, struct rdrmnull_scanout *s)
{
	(void) memset(s, 0, sizeof (*s));
	return (xioctl(fd, RDRMNULL_IOCTL_SCANOUT, s) == 0);
}

/* The ID of property `name` of an object, or 0. */
static uint32_t
prop_id(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
	drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0, i;

	if (p == NULL)
		return (0);
	for (i = 0; i < p->count_props && id == 0; i++) {
		drmModePropertyPtr d = drmModeGetProperty(fd, p->props[i]);
		if (d != NULL && strcmp(d->name, name) == 0) {
			id = d->prop_id;
			if (value != NULL)
				*value = p->prop_values[i];
		}
		drmModeFreeProperty(d);
	}
	drmModeFreeObjectProperties(p);
	return (id);
}

/* The ID of a property by name among IDs 1..200, attached or not. */
static uint32_t
find_prop(int fd, const char *name)
{
	uint32_t id;

	for (id = 1; id < 200; id++) {
		drmModePropertyPtr d = drmModeGetProperty(fd, id);
		int hit = d != NULL && strcmp(d->name, name) == 0;
		drmModeFreeProperty(d);
		if (hit)
			return (id);
	}
	return (0);
}

/* Waits for one event and reads it; returns the bytes read or -errno. */
static int
read_event(int fd, int timeout_ms, void *out, size_t len)
{
	struct pollfd p = { fd, POLLIN, 0 };
	int n = poll(&p, 1, timeout_ms);
	ssize_t r;

	if (n <= 0)
		return (n == 0 ? -ETIMEDOUT : -errno);
	if (!(p.revents & POLLIN))
		return (-EIO);
	r = read(fd, out, len);
	return (r < 0 ? -errno : (int)r);
}

/* The state the tests build on. */
static uint32_t crtc_id, conn_id, enc_id, primary_id, cursor_id;
static drmModeModeInfo mode1024;

/* --- tests --- */

static void
test_caps(int fd)
{
	uint64_t v;

	v = 0;
	check(drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &v) == 0 && v == 1, "DRM_CAP_ADDFB2_MODIFIERS is 1");
	v = 0;
	check(drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &v) == 0 && v == 1, "DRM_CAP_CRTC_IN_VBLANK_EVENT is 1");
	v = 0;
	check(drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &v) == 0 && v == 1, "DRM_CAP_TIMESTAMP_MONOTONIC is 1");
	v = 7;
	check(drmGetCap(fd, DRM_CAP_ASYNC_PAGE_FLIP, &v) == 0 && v == 0, "DRM_CAP_ASYNC_PAGE_FLIP is 0");
}

static void
test_resources(int fd, int rfd)
{
	drmModeResPtr r = drmModeGetResources(fd);
	drmModePlaneResPtr pr;
	drmModeConnectorPtr c;
	drmModeEncoderPtr e;
	struct drm_mode_card_res raw;
	uint32_t i, crtcs[4];
	char what[160];

	check(r != NULL && r->count_crtcs == 1 && r->count_connectors == 1 && r->count_encoders == 1 &&
	    r->count_fbs == 0, "GETRESOURCES: one CRTC, one connector, one encoder, no framebuffers");
	if (r == NULL)
		return;
	check(r->min_width == 1 && r->max_width == 2048 && r->min_height == 1 && r->max_height == 2048,
	    "GETRESOURCES: framebuffer limits 1..2048");
	crtc_id = r->crtcs[0];
	conn_id = r->connectors[0];
	enc_id = r->encoders[0];
	drmModeFreeResources(r);
	/* A short array gets a partial copy and the full count. */
	(void) memset(&raw, 0, sizeof (raw));
	raw.count_crtcs = 0;
	raw.crtc_id_ptr = (uintptr_t)crtcs;
	crtcs[0] = 0xdead;
	check(xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &raw) == 0 && raw.count_crtcs == 1 &&
	    crtcs[0] == 0xdead, "GETRESOURCES with no room reports the count and copies nothing");
	(void) memset(&raw, 0, sizeof (raw));
	check(xioctl(rfd, DRM_IOCTL_MODE_GETRESOURCES, &raw) == EACCES,
	    "GETRESOURCES on the render node is EACCES");

	pr = drmModeGetPlaneResources(fd);
	check(pr != NULL && pr->count_planes == 0, "GETPLANERESOURCES without universal planes: no overlays");
	drmModeFreePlaneResources(pr);
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0, "SET_CLIENT_CAP UNIVERSAL_PLANES");
	pr = drmModeGetPlaneResources(fd);
	check(pr != NULL && pr->count_planes == 2, "GETPLANERESOURCES with universal planes: two planes");
	if (pr != NULL) {
		for (i = 0; i < pr->count_planes; i++) {
			uint64_t t = 99;
			drmModePlanePtr p = drmModeGetPlane(fd, pr->planes[i]);
			(void) prop_id(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE, "type", &t);
			if (t == 1)
				primary_id = pr->planes[i];
			if (t == 2)
				cursor_id = pr->planes[i];
			(void) snprintf(what, sizeof (what), "GETPLANE %u: type %llu, possible_crtcs 1, %u formats",
			    pr->planes[i], (unsigned long long)t, p != NULL ? p->count_formats : 0);
			check(p != NULL && p->possible_crtcs == 1 && p->count_formats >= 1 && p->crtc_id == 0,
			    what);
			drmModeFreePlane(p);
		}
		drmModeFreePlaneResources(pr);
	}
	check(primary_id != 0 && cursor_id != 0, "a primary and a cursor plane");

	c = drmModeGetConnector(fd, conn_id);
	check(c != NULL && c->connection == DRM_MODE_CONNECTED && c->connector_type == DRM_MODE_CONNECTOR_VIRTUAL &&
	    c->connector_type_id == 1, "GETCONNECTOR: a connected virtual connector, type id 1");
	if (c != NULL) {
		int pref = c->count_modes > 0 && c->modes[0].hdisplay == 1024 && c->modes[0].vdisplay == 768 &&
		    (c->modes[0].type & DRM_MODE_TYPE_PREFERRED) && c->modes[0].vrefresh == 60;
		(void) snprintf(what, sizeof (what), "GETCONNECTOR: %d modes from the EDID, 1024x768@60 preferred first",
		    c->count_modes);
		check(c->count_modes >= 4 && pref, what);
		for (i = 0; i < (uint32_t)c->count_modes; i++)
			(void) printf("INFO mode %u: %s %ux%u@%u clock %u type 0x%x\n", i, c->modes[i].name,
			    c->modes[i].hdisplay, c->modes[i].vdisplay, c->modes[i].vrefresh,
			    c->modes[i].clock, c->modes[i].type);
		if (pref)
			mode1024 = c->modes[0];
		check(c->mmWidth == 340 && c->mmHeight == 270, "GETCONNECTOR: 340 by 270 mm from the EDID");
		check(c->count_encoders == 1 && c->encoders[0] == enc_id && c->encoder_id == 0,
		    "GETCONNECTOR: one possible encoder, none in use");
		drmModeFreeConnector(c);
	}
	e = drmModeGetEncoder(fd, enc_id);
	check(e != NULL && e->encoder_type == DRM_MODE_ENCODER_VIRTUAL && e->possible_crtcs == 1 && e->crtc_id == 0,
	    "GETENCODER: virtual, possible_crtcs 1, idle");
	drmModeFreeEncoder(e);
	check(drmModeGetConnector(fd, 4242) == NULL && errno == ENOENT, "GETCONNECTOR of an unknown ID is ENOENT");
}

static void
test_properties(int fd)
{
	uint64_t edid_id = 0, v = 0;
	drmModePropertyBlobPtr blob;
	drmModePropertyPtr p;
	uint32_t id;

	id = prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", &edid_id);
	check(id != 0 && edid_id != 0, "connector property EDID names a blob");
	blob = drmModeGetPropertyBlob(fd, (uint32_t)edid_id);
	check(blob != NULL && blob->length == 128 &&
	    memcmp(blob->data, "\0\377\377\377\377\377\377\0", 8) == 0, "GETPROPBLOB: a 128-byte EDID");
	drmModeFreePropertyBlob(blob);
	p = drmModeGetProperty(fd, id);
	check(p != NULL && (p->flags & DRM_MODE_PROP_BLOB) && (p->flags & DRM_MODE_PROP_IMMUTABLE),
	    "GETPROPERTY EDID: an immutable blob property");
	drmModeFreeProperty(p);
	id = prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS", &v);
	p = drmModeGetProperty(fd, id);
	check(p != NULL && (p->flags & DRM_MODE_PROP_ENUM) && p->count_enums == 4 &&
	    strcmp(p->enums[3].name, "Off") == 0 && v == DRM_MODE_DPMS_OFF,
	    "GETPROPERTY DPMS: four enums, currently Off");
	drmModeFreeProperty(p);
	check(prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL) == 0,
	    "a non-atomic client does not see the atomic CRTC_ID");
	check(prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL) == 0,
	    "nor ACTIVE");
	p = drmModeGetProperty(fd, find_prop(fd, "CRTC_X"));
	check(p != NULL && (p->flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_SIGNED_RANGE &&
	    p->count_values == 2 && (int64_t)p->values[0] == -2147483648LL,
	    "GETPROPERTY CRTC_X: a signed range from INT_MIN");
	drmModeFreeProperty(p);
	/* IN_FORMATS: XRGB8888 and friends with the linear modifier. */
	id = prop_id(fd, primary_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &v);
	blob = drmModeGetPropertyBlob(fd, (uint32_t)v);
	if (blob != NULL && blob->length >= 24) {
		const uint32_t *h = blob->data;
		const uint32_t *fmts = (const uint32_t *)((const char *)blob->data + h[3]);
		const uint64_t *mods = (const uint64_t *)((const char *)blob->data + h[5]);
		check(h[0] == 1 && h[2] == 3 && fmts[0] == XRGB8888 && h[4] == 1 && mods[0] == 7 &&
		    mods[2] == MOD_LINEAR, "IN_FORMATS: three formats, all linear");
	} else {
		check(0, "IN_FORMATS blob of the primary plane");
	}
	drmModeFreePropertyBlob(blob);
	check(drmModeGetPropertyBlob(fd, 4242) == NULL && errno == ENOENT, "GETPROPBLOB of an unknown ID is ENOENT");
}

static void
test_framebuffers(int fd, int fd2)
{
	struct drm_mode_fb_cmd old;
	struct drm_mode_fb_cmd2 f2;
	struct buf b, small;
	uint32_t fb = 0, fb2 = 0, fbx = 0;
	drmModeFBPtr info;
	drmModeResPtr r;

	check(buf_create(fd, 256, 128, 32, &b), "dumb buffer 256x128");
	check(buf_create(fd, 16, 16, 32, &small), "dumb buffer 16x16");
	check(addfb2(fd, 256, 128, XRGB8888, b.handle, b.pitch, DRM_MODE_FB_MODIFIERS, MOD_X_TILED, NULL) == EINVAL,
	    "ADDFB2 with an unsupported modifier (X tiled) is EINVAL");
	check(addfb2(fd, 256, 128, NV12, b.handle, b.pitch, 0, 0, NULL) == EINVAL,
	    "ADDFB2 with a format no plane scans out (NV12) is EINVAL");
	check(addfb2(fd, 256, 128, FOURCC('Y', 'U', 'Y', 'V'), b.handle, b.pitch, 0, 0, NULL) == EINVAL,
	    "ADDFB2 with an unknown format is EINVAL");
	check(addfb2(fd, 256, 128, XRGB8888, 4242, b.pitch, 0, 0, NULL) == ENOENT,
	    "ADDFB2 with an unknown handle is ENOENT");
	check(addfb2(fd, 256, 128, XRGB8888, b.handle, 256 * 4 - 1, 0, 0, NULL) == EINVAL,
	    "ADDFB2 with a pitch below a line is EINVAL");
	check(addfb2(fd, 256, 128, XRGB8888, small.handle, 1024, 0, 0, NULL) == EINVAL,
	    "ADDFB2 on an object too small for the image is EINVAL");
	check(addfb2(fd, 4096, 16, XRGB8888, b.handle, 16384, 0, 0, NULL) == EINVAL,
	    "ADDFB2 wider than max_width is EINVAL");
	check(addfb2(fd, 256, 128, XRGB8888, b.handle, b.pitch, DRM_MODE_FB_MODIFIERS, MOD_LINEAR, &fb2) == 0,
	    "ADDFB2 XRGB8888 with the linear modifier");
	(void) memset(&old, 0, sizeof (old));
	old.width = 256;
	old.height = 128;
	old.pitch = b.pitch;
	old.bpp = 32;
	old.depth = 24;
	old.handle = b.handle;
	check(xioctl(fd, DRM_IOCTL_MODE_ADDFB, &old) == 0 && old.fb_id != 0, "legacy ADDFB 32/24");
	fb = old.fb_id;
	old.depth = 16;
	check(xioctl(fd, DRM_IOCTL_MODE_ADDFB, &old) == EINVAL, "legacy ADDFB 32/16 is EINVAL");
	info = drmModeGetFB(fd, fb);
	check(info != NULL && info->width == 256 && info->bpp == 32 && info->depth == 24 && info->handle != 0 &&
	    info->pitch == b.pitch, "GETFB as master: size, bpp 32, depth 24 and a new handle");
	if (info != NULL && info->handle != 0 && info->handle != b.handle) {
		struct drm_gem_close gc = { info->handle, 0 };
		(void) xioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	drmModeFreeFB(info);
	info = drmModeGetFB(fd2, fb);
	check(info != NULL && info->handle != 0, "GETFB from a non-master root file: a handle (PRIV_SYS_DEVICES)");
	if (info != NULL && info->handle != 0) {
		struct drm_gem_close gc = { info->handle, 0 };
		(void) xioctl(fd2, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	drmModeFreeFB(info);
	{
		pid_t pid = fork();
		int st = -1;
		if (pid == 0) {
			int nfd;
			if (setuid(60001) != 0 || (nfd = open(CARD, O_RDWR)) < 0)
				_exit(2);
			info = drmModeGetFB(nfd, fb);
			_exit(info != NULL && info->handle == 0 && info->width == 256 ? 0 : 1);
		}
		(void) waitpid(pid, &st, 0);
		check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "GETFB from an unprivileged non-master: no handle");
	}
	(void) memset(&f2, 0, sizeof (f2));
	f2.fb_id = fb2;
	check(xioctl(fd, DRM_IOCTL_MODE_GETFB2, &f2) == 0 && f2.pixel_format == XRGB8888 &&
	    (f2.flags & DRM_MODE_FB_MODIFIERS) && f2.modifier[0] == MOD_LINEAR && f2.handles[0] != 0 &&
	    f2.pitches[0] == b.pitch && f2.handles[1] == 0, "GETFB2: format, modifier flag, linear, one handle");
	if (f2.handles[0] != 0 && f2.handles[0] != b.handle) {
		struct drm_gem_close gc = { f2.handles[0], 0 };
		(void) xioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	r = drmModeGetResources(fd);
	check(r != NULL && r->count_fbs == 2 && r->fbs[0] == fb && r->fbs[1] == fb2,
	    "GETRESOURCES lists the file's two framebuffers, newest first");
	drmModeFreeResources(r);
	r = drmModeGetResources(fd2);
	check(r != NULL && r->count_fbs == 0, "another file sees none of them");
	drmModeFreeResources(r);
	check(rmfb(fd2, fb) == ENOENT, "RMFB of another file's framebuffer is ENOENT");
	check(rmfb(fd, 4242) == ENOENT, "RMFB of an unknown ID is ENOENT");
	check(rmfb(fd, fb) == 0 && rmfb(fd, fb2) == 0, "RMFB of both");
	check(rmfb(fd, fb) == ENOENT, "RMFB twice is ENOENT");
	/* A framebuffer keeps its object after the handle is gone. */
	check(addfb2(fd, 16, 16, XRGB8888, small.handle, small.pitch, 0, 0, &fbx) == 0, "ADDFB2 16x16");
	buf_destroy(fd, &small);
	info = drmModeGetFB(fd, fbx);
	check(info != NULL && info->width == 16, "the framebuffer survives DESTROY_DUMB of its handle");
	if (info != NULL && info->handle != 0) {
		struct drm_gem_close gc = { info->handle, 0 };
		(void) xioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	drmModeFreeFB(info);
	check(rmfb(fd, fbx) == 0, "RMFB frees it");
	buf_destroy(fd, &b);
}

static void
test_blobs(int fd, int fd2)
{
	struct drm_mode_create_blob c;
	struct drm_mode_destroy_blob d;
	struct drm_mode_get_blob g;
	char data[100], back[100];

	(void) memset(&c, 0, sizeof (c));
	c.data = (uintptr_t)data;
	c.length = 0;
	check(xioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &c) == EINVAL, "CREATEPROPBLOB of 0 bytes is EINVAL");
	c.length = 2 << 20;
	check(xioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &c) == ENOMEM, "CREATEPROPBLOB above 1 MiB is ENOMEM");
	(void) memset(data, 0x5a, sizeof (data));
	c.length = sizeof (data);
	check(xioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &c) == 0 && c.blob_id != 0, "CREATEPROPBLOB of 100 bytes");
	(void) memset(&g, 0, sizeof (g));
	g.blob_id = c.blob_id;
	g.data = (uintptr_t)back;
	g.length = 99;
	(void) memset(back, 0, sizeof (back));
	check(xioctl(fd2, DRM_IOCTL_MODE_GETPROPBLOB, &g) == 0 && g.length == 100 && back[0] == 0,
	    "GETPROPBLOB with a short length returns the length only (any file)");
	check(xioctl(fd2, DRM_IOCTL_MODE_GETPROPBLOB, &g) == 0 && memcmp(back, data, 100) == 0,
	    "GETPROPBLOB with the right length returns the data");
	(void) memset(&d, 0, sizeof (d));
	d.blob_id = c.blob_id;
	check(xioctl(fd2, DRM_IOCTL_MODE_DESTROYPROPBLOB, &d) == EPERM, "DESTROYPROPBLOB by another file is EPERM");
	check(xioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &d) == 0, "DESTROYPROPBLOB by its creator");
	check(xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &g) == ENOENT, "the blob is gone");
	d.blob_id = 4242;
	check(xioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &d) == ENOENT, "DESTROYPROPBLOB of an unknown ID is ENOENT");
}

/* Waits until the scanout shows `fb` or a second passes. */
static int
wait_scanout(int fd, uint32_t fb, struct rdrmnull_scanout *s)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (scanout(fd, s) && s->fb_id == fb)
			return (1);
		msleep(10);
	}
	return (0);
}

static void
test_legacy(int fd, int fd2)
{
	struct rdrmnull_scanout s;
	struct buf a, b, cur, big;
	uint32_t fa = 0, fb = 0, f16 = 0, conn = conn_id;
	struct drm_mode_crtc_page_flip pf;
	drmModeCrtcPtr crtc;
	drmModeEncoderPtr e;
	char ev[64];
	int n;
	uint16_t red[256], green[256], blue[256];
	struct drm_event_vblank *v = (struct drm_event_vblank *)ev;

	check(buf_create(fd, 1024, 768, 32, &a) && buf_create(fd, 1024, 768, 32, &b), "two 1024x768 dumb buffers");
	buf_fill(&a, 0x1111);
	buf_fill(&b, 0x2222);
	check(addfb2(fd, 1024, 768, XRGB8888, a.handle, a.pitch, 0, 0, &fa) == 0 &&
	    addfb2(fd, 1024, 768, XRGB8888, b.handle, b.pitch, 0, 0, &fb) == 0, "ADDFB2 of both");
	check(drmModeSetCrtc(fd2, crtc_id, fa, 0, 0, &conn, 1, &mode1024) == -EACCES,
	    "SETCRTC from a non-master file is EACCES");
	check(drmModeSetCrtc(fd, crtc_id, 0xffffffff, 0, 0, &conn, 1, &mode1024) == -EINVAL,
	    "SETCRTC with fb -1 and no current framebuffer is EINVAL");
	check(drmModeSetCrtc(fd, crtc_id, fa, 1, 0, &conn, 1, &mode1024) == -ENOSPC,
	    "SETCRTC with a viewport past the framebuffer is ENOSPC");
	check(drmModeSetCrtc(fd, crtc_id, fa, 0, 0, NULL, 0, &mode1024) == -EINVAL,
	    "SETCRTC with a mode and no connectors is EINVAL");
	{
		drmModeModeInfo odd = mode1024;
		odd.vscan = 2;
		check(drmModeSetCrtc(fd, crtc_id, fa, 0, 0, &conn, 1, &odd) == -EINVAL,
		    "SETCRTC with a mode the display cannot show (vscan 2) is EINVAL");
	}
	check(drmModeSetCrtc(fd, crtc_id, fa, 0, 0, &conn, 1, &mode1024) == 0, "SETCRTC 1024x768 with framebuffer A");
	crtc = drmModeGetCrtc(fd, crtc_id);
	check(crtc != NULL && crtc->mode_valid && crtc->buffer_id == fa && crtc->mode.hdisplay == 1024 &&
	    crtc->gamma_size == 256, "GETCRTC: mode valid, framebuffer A, gamma 256");
	drmModeFreeCrtc(crtc);
	e = drmModeGetEncoder(fd, enc_id);
	check(e != NULL && e->crtc_id == crtc_id, "GETENCODER: driven by the CRTC now");
	drmModeFreeEncoder(e);
	check(wait_scanout(fd, fa, &s) && s.active && s.width == 1024 && s.height == 768 &&
	    s.checksum == fnv(&a, 0, 0, 1024, 768, 4), "the scanout shows A (checksum matches)");
	(void) printf("INFO scanout A checksum %016llx frames %llu\n", (unsigned long long)s.checksum,
	    (unsigned long long)s.frames);

	/* A page flip with an event: poll, read, then the scanout shows B. */
	check(drmModePageFlip(fd, crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, (void *)0x1234) == 0, "PAGE_FLIP to B");
	check(drmModePageFlip(fd, crtc_id, fa, 0, NULL) == -EBUSY,
	    "a second PAGE_FLIP before the first completed is EBUSY");
	n = read_event(fd, 1000, ev, sizeof (ev));
	check(n == 32 && v->base.type == DRM_EVENT_FLIP_COMPLETE && v->base.length == 32 &&
	    v->user_data == 0x1234 && v->crtc_id == crtc_id && v->sequence > 0,
	    "poll + read: one FLIP_COMPLETE event with user data and CRTC ID");
	check(scanout(fd, &s) && s.fb_id == fb && s.checksum == fnv(&b, 0, 0, 1024, 768, 4),
	    "after the event the scanout shows B");
	/* Page flip argument errors. */
	check(addfb2(fd, 1024, 768, RGB565, a.handle, a.pitch, 0, 0, &f16) == 0, "ADDFB2 RGB565");
	check(drmModePageFlip(fd, crtc_id, f16, 0, NULL) == -EINVAL,
	    "PAGE_FLIP to another format is EINVAL");
	check(drmModePageFlip(fd, crtc_id, fa, DRM_MODE_PAGE_FLIP_ASYNC, NULL) == -EINVAL,
	    "PAGE_FLIP_ASYNC is EINVAL");
	(void) memset(&pf, 0, sizeof (pf));
	pf.crtc_id = crtc_id;
	pf.fb_id = fa;
	pf.flags = 0x80;
	check(xioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == EINVAL, "PAGE_FLIP with unknown flags is EINVAL");
	check(drmModePageFlip(fd, crtc_id, 4242, 0, NULL) == -ENOENT,
	    "PAGE_FLIP to an unknown framebuffer is ENOENT");
	(void) rmfb(fd, f16);

	/* The cursor. */
	check(buf_create(fd, 64, 64, 32, &cur) && buf_create(fd, 128, 128, 32, &big), "cursor buffers");
	check(drmModeSetCursor(fd, crtc_id, cur.handle, 64, 64) == 0, "CURSOR: a 64x64 image");
	check(drmModeMoveCursor(fd, crtc_id, 100, 50) == 0, "CURSOR: move");
	{
		uint64_t x = 0, fbid = 0;
		(void) drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
		(void) prop_id(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &x);
		(void) prop_id(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &fbid);
		(void) drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 0);
		(void) drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
		check(x == 100 && fbid != 0, "the cursor plane is at x 100 with an internal framebuffer");
	}
	check(drmModeSetCursor2(fd, crtc_id, cur.handle, 64, 64, 3, 4) == 0, "CURSOR2 with a hotspot");
	check(drmModeSetCursor(fd, crtc_id, big.handle, 128, 128) == -EINVAL,
	    "CURSOR: 128x128 is refused by the driver (EINVAL)");
	check(drmModeSetCursor(fd, crtc_id, 0, 0, 0) == 0, "CURSOR: hide");
	check(drmModeSetCursor(fd, 4242, 0, 0, 0) == -ENOENT, "CURSOR on an unknown CRTC is ENOENT");
	buf_destroy(fd, &big);
	buf_destroy(fd, &cur);

	/* Gamma. */
	check(drmModeCrtcGetGamma(fd, crtc_id, 256, red, green, blue) == 0 && red[0] == 0 && red[255] == 0xffff,
	    "GETGAMMA: a linear ramp of 256");
	check(drmModeCrtcGetGamma(fd, crtc_id, 255, red, green, blue) == -EINVAL,
	    "GETGAMMA with the wrong size is EINVAL");
	red[10] = 7;
	check(drmModeCrtcSetGamma(fd, crtc_id, 256, red, green, blue) == 0, "SETGAMMA");
	(void) memset(red, 0, sizeof (red));
	check(drmModeCrtcGetGamma(fd, crtc_id, 256, red, green, blue) == 0 && red[10] == 7, "GETGAMMA reads it back");
	check(drmModeCrtcSetGamma(fd2, crtc_id, 256, red, green, blue) == -EACCES,
	    "SETGAMMA from a non-master file is EACCES");

	/* DPMS through the legacy property. */
	{
		uint32_t dpms = find_prop(fd, "DPMS");
		uint64_t val = 9;
		check(drmModeConnectorSetProperty(fd, conn_id, dpms, DRM_MODE_DPMS_OFF) == 0, "SETPROPERTY DPMS Off");
		(void) prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS", &val);
		check(val == DRM_MODE_DPMS_OFF && scanout(fd, &s) && !s.active,
		    "DPMS reads back Off and the CRTC is inactive");
		check(drmModeConnectorSetProperty(fd, conn_id, dpms, DRM_MODE_DPMS_ON) == 0, "SETPROPERTY DPMS On");
		(void) prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS", &val);
		check(val == DRM_MODE_DPMS_ON && scanout(fd, &s) && s.active, "DPMS reads back On and the CRTC is active");
		check(drmModeConnectorSetProperty(fd, conn_id, find_prop(fd, "EDID"), 0) == -EINVAL,
		    "SETPROPERTY of the immutable EDID is EINVAL");
	}

	/* RMFB of the framebuffer on screen turns the CRTC off. */
	check(drmModeSetCrtc(fd, crtc_id, fa, 0, 0, &conn, 1, &mode1024) == 0, "SETCRTC back to A");
	check(rmfb(fd, fa) == 0, "RMFB of the framebuffer on screen");
	crtc = drmModeGetCrtc(fd, crtc_id);
	check(crtc != NULL && !crtc->mode_valid && crtc->buffer_id == 0, "the CRTC is off after RMFB");
	drmModeFreeCrtc(crtc);
	check(scanout(fd, &s) && !s.active && s.fb_id == 0, "and the scanout shows nothing");
	(void) rmfb(fd, fb);
	buf_destroy(fd, &a);
	buf_destroy(fd, &b);
}

/* One commit through the atomic ioctl. */
struct commit {
	uint32_t objs[8], counts[8], props[32];
	uint64_t values[32];
	int nobj, nprop;
};

static void
add(struct commit *c, uint32_t obj, uint32_t prop, uint64_t value)
{
	if (c->nobj == 0 || c->objs[c->nobj - 1] != obj) {
		c->objs[c->nobj] = obj;
		c->counts[c->nobj++] = 0;
	}
	c->counts[c->nobj - 1]++;
	c->props[c->nprop] = prop;
	c->values[c->nprop++] = value;
}

static int
commit(int fd, struct commit *c, uint32_t flags, uint64_t user)
{
	struct drm_mode_atomic a;

	(void) memset(&a, 0, sizeof (a));
	a.flags = flags;
	a.count_objs = c->nobj;
	a.objs_ptr = (uintptr_t)c->objs;
	a.count_props_ptr = (uintptr_t)c->counts;
	a.props_ptr = (uintptr_t)c->props;
	a.prop_values_ptr = (uintptr_t)c->values;
	a.user_data = user;
	return (xioctl(fd, DRM_IOCTL_MODE_ATOMIC, &a));
}

static uint32_t
P(int fd, uint32_t obj, uint32_t type, const char *name)
{
	return (prop_id(fd, obj, type, name, NULL));
}

static void
test_atomic(int fd, int fd2)
{
	struct rdrmnull_scanout s;
	struct commit c;
	struct buf a, b;
	uint32_t fa = 0, fb = 0, mode_blob = 0, out_fence, in_fence;
	uint64_t val;
	char ev[64];
	struct drm_event_vblank *v = (struct drm_event_vblank *)ev;
	int n;

	(void) memset(&c, 0, sizeof (c));
	check(commit(fd, &c, 0, 0) == EINVAL, "ATOMIC without the atomic client capability is EINVAL");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0, "SET_CLIENT_CAP ATOMIC");
	check(P(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID") != 0 &&
	    P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID") != 0 &&
	    P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "SRC_W") != 0, "an atomic client sees CRTC_ID, MODE_ID, SRC_W");
	check(buf_create(fd, 1024, 768, 32, &a) && buf_create(fd, 1024, 768, 32, &b), "two buffers");
	buf_fill(&a, 0xa1a1);
	buf_fill(&b, 0xb2b2);
	(void) addfb2(fd, 1024, 768, XRGB8888, a.handle, a.pitch, 0, 0, &fa);
	(void) addfb2(fd, 1024, 768, XRGB8888, b.handle, b.pitch, 0, 0, &fb);
	check(drmModeCreatePropertyBlob(fd, &mode1024, sizeof (mode1024), &mode_blob) == 0, "a MODE_ID blob");

#define	FULL(c, fbid) do { \
	(void) memset(&(c), 0, sizeof (c)); \
	add(&(c), conn_id, P(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), crtc_id); \
	add(&(c), crtc_id, P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), mode_blob); \
	add(&(c), crtc_id, P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1); \
	add(&(c), primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), fbid); \
	add(&(c), primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), crtc_id); \
	add(&(c), primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), 1024 << 16); \
	add(&(c), primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "SRC_H"), 768 << 16); \
	add(&(c), primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"), 1024); \
	add(&(c), primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "CRTC_H"), 768); \
	} while (0)

	FULL(c, fa);
	check(commit(fd, &c, DRM_MODE_ATOMIC_TEST_ONLY, 0) == EINVAL,
	    "TEST_ONLY of a full modeset without ALLOW_MODESET is EINVAL");
	check(commit(fd, &c, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == 0,
	    "TEST_ONLY with ALLOW_MODESET passes");
	check(scanout(fd, &s) && !s.active && s.fb_id == 0, "TEST_ONLY changed nothing");
	check(commit(fd, &c, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_PAGE_FLIP_EVENT, 0) == EINVAL,
	    "TEST_ONLY with PAGE_FLIP_EVENT is EINVAL");
	check(commit(fd2, &c, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == EACCES, "ATOMIC from a non-master file is EACCES");
	check(commit(fd, &c, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, 0xabc) == 0,
	    "blocking modeset commit with a flip event");
	n = read_event(fd, 1000, ev, sizeof (ev));
	check(n == 32 && v->base.type == DRM_EVENT_FLIP_COMPLETE && v->user_data == 0xabc && v->crtc_id == crtc_id,
	    "its FLIP_COMPLETE event");
	check(scanout(fd, &s) && s.active && s.fb_id == fa && s.checksum == fnv(&a, 0, 0, 1024, 768, 4),
	    "the scanout shows A");
	/* A flip to B: TEST_ONLY first, then real, non-blocking, with an event. */
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), fb);
	check(commit(fd, &c, DRM_MODE_ATOMIC_TEST_ONLY, 0) == 0, "TEST_ONLY flip to B passes");
	check(scanout(fd, &s) && s.fb_id == fa, "and changes nothing");
	check(commit(fd, &c, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, 0xdef) == 0,
	    "NONBLOCK flip to B with an event");
	check(commit(fd, &c, DRM_MODE_ATOMIC_NONBLOCK, 0) == EBUSY, "a second NONBLOCK commit while pending is EBUSY");
	n = read_event(fd, 1000, ev, sizeof (ev));
	check(n == 32 && v->user_data == 0xdef && v->base.type == DRM_EVENT_FLIP_COMPLETE, "poll + read its event");
	check(scanout(fd, &s) && s.fb_id == fb && s.checksum == fnv(&b, 0, 0, 1024, 768, 4),
	    "the scanout shows B, checksum matches");
	/* Errors. */
	(void) memset(&c, 0, sizeof (c));
	add(&c, crtc_id, P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), 0);
	check(commit(fd, &c, DRM_MODE_ATOMIC_ALLOW_MODESET, 0) == EINVAL,
	    "removing the mode of an active CRTC is EINVAL (ACTIVE without a mode)");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), 2048u << 16);
	check(commit(fd, &c, 0, 0) == ENOSPC, "a source rectangle outside the framebuffer is ENOSPC");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "CRTC_X"), 5);
	check(commit(fd, &c, 0, 0) == EINVAL, "moving the primary plane is refused by the driver (EINVAL)");
	(void) memset(&c, 0, sizeof (c));
	add(&c, 4242, 1, 0);
	check(commit(fd, &c, 0, 0) == ENOENT, "an unknown object is ENOENT");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
	check(commit(fd, &c, 0, 0) == ENOENT, "a property the object does not have is ENOENT");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "type"), 2);
	check(commit(fd, &c, 0, 0) == EINVAL, "writing the immutable type is EINVAL");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), 4242);
	check(commit(fd, &c, 0, 0) == EINVAL, "FB_ID of an unknown framebuffer is EINVAL");
	out_fence = find_prop(fd, "OUT_FENCE_PTR");
	in_fence = find_prop(fd, "IN_FENCE_FD");
	check(out_fence != 0 && in_fence != 0 && P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR") == 0 &&
	    P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD") == 0,
	    "fence properties exist but no object carries them (R2)");
	(void) memset(&c, 0, sizeof (c));
	add(&c, crtc_id, out_fence, (uintptr_t)&val);
	check(commit(fd, &c, 0, 0) == EINVAL, "OUT_FENCE_PTR is refused with EINVAL");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, in_fence, 3);
	check(commit(fd, &c, 0, 0) == EINVAL, "IN_FENCE_FD is refused with EINVAL");
	(void) memset(&c, 0, sizeof (c));
	add(&c, primary_id, P(fd, primary_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), fa);
	check(commit(fd, &c, DRM_MODE_PAGE_FLIP_ASYNC, 0) == EINVAL, "PAGE_FLIP_ASYNC in ATOMIC is EINVAL");
	check(commit(fd, &c, 0x8000, 0) == EINVAL, "unknown ATOMIC flags are EINVAL");
	check(scanout(fd, &s) && s.fb_id == fb, "none of the failed commits changed the screen");

	/* Property get/set round trip on the cursor plane and the connector. */
	(void) memset(&c, 0, sizeof (c));
	{
		struct buf cur;
		uint32_t fc = 0;
		check(buf_create(fd, 64, 64, 32, &cur) &&
		    addfb2(fd, 64, 64, ARGB8888, cur.handle, cur.pitch, 0, 0, &fc) == 0, "an ARGB8888 cursor");
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), fc);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), crtc_id);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_X"), (uint64_t)(int64_t)-10);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 300);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"), 64);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_H"), 64);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), 64 << 16);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "SRC_H"), 64 << 16);
		check(commit(fd, &c, 0, 0) == 0, "atomic cursor plane update at (-10, 300)");
		val = 0;
		(void) prop_id(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &val);
		check((int64_t)val == -10, "CRTC_X reads back -10");
		(void) prop_id(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &val);
		check(val == fc, "FB_ID reads back the cursor framebuffer");
		check(drmModeObjectSetProperty(fd, cursor_id, DRM_MODE_OBJECT_PLANE,
		    P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 400) == 0, "OBJ_SETPROPERTY CRTC_Y 400");
		(void) prop_id(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &val);
		check(val == 400, "CRTC_Y reads back 400");
		check(drmModeObjectSetProperty(fd, cursor_id, DRM_MODE_OBJECT_CRTC,
		    P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 1) == -ENOENT,
		    "OBJ_SETPROPERTY with the wrong object type is ENOENT");
		check(drmModeObjectSetProperty(fd, cursor_id, DRM_MODE_OBJECT_PLANE,
		    P(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1) == -EINVAL,
		    "OBJ_SETPROPERTY of a property the object lacks is EINVAL");
		check(drmModeObjectSetProperty(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,
		    P(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "link-status"), DRM_MODE_LINK_STATUS_GOOD) == 0,
		    "OBJ_SETPROPERTY link-status Good");
		(void) prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &val);
		check(val == 1, "ACTIVE reads back 1");
		(void) memset(&c, 0, sizeof (c));
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), 0);
		add(&c, cursor_id, P(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), 0);
		check(commit(fd, &c, 0, 0) == 0, "turn the cursor plane off");
		(void) rmfb(fd, fc);
		buf_destroy(fd, &cur);
	}
	/* Leave the display on with B for the vblank tests. */
	(void) rmfb(fd, fa);
	buf_destroy(fd, &a);
	(void) drmModeDestroyPropertyBlob(fd, mode_blob);
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 0) == 0, "SET_CLIENT_CAP ATOMIC 0");
	/*
	 * B stays on screen for the vblank tests; its handle and framebuffer
	 * go when the file closes, but the mapping must go now, since a
	 * mapping keeps the file open.
	 */
	(void) munmap((void *)b.map, b.size);
}

static void
test_vblank(int fd)
{
	drmVBlank vbl;
	struct drm_crtc_get_sequence gs;
	struct drm_crtc_queue_sequence qs;
	uint32_t seq0;
	double t0, t1, rate;
	char ev[64];
	struct drm_event_vblank *v = (struct drm_event_vblank *)ev;
	struct drm_event_crtc_sequence *sq = (struct drm_event_crtc_sequence *)ev;
	uint64_t tprev = 0, dmin = ~0ULL, dmax = 0;
	int i, n, ok = 1;
	char what[160];

	(void) memset(&vbl, 0, sizeof (vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.sequence = 0;
	check(drmWaitVBlank(fd, &vbl) == 0 && vbl.reply.sequence > 0, "WAIT_VBLANK query");
	seq0 = vbl.reply.sequence;
	t0 = now_s();
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.sequence = 60;
	check(drmWaitVBlank(fd, &vbl) == 0 && vbl.reply.sequence - seq0 >= 60, "WAIT_VBLANK for 60 vblanks");
	t1 = now_s();
	rate = 60.0 / (t1 - t0);
	(void) snprintf(what, sizeof (what), "vblank rate %.2f Hz over 60 frames (1024x768@60: 60.00 Hz nominal)", rate);
	check(rate > 55.0 && rate < 65.0, what);
	/* Timestamps: the reply of each wait for the next vblank. */
	for (i = 0; i < 30; i++) {
		uint64_t t;
		vbl.request.type = DRM_VBLANK_RELATIVE;
		vbl.request.sequence = 1;
		if (drmWaitVBlank(fd, &vbl) != 0) {
			ok = 0;
			break;
		}
		t = (uint64_t)vbl.reply.tval_sec * 1000000 + vbl.reply.tval_usec;
		if (tprev != 0) {
			if (t - tprev < dmin)
				dmin = t - tprev;
			if (t - tprev > dmax)
				dmax = t - tprev;
		}
		tprev = t;
	}
	(void) printf("INFO vblank timestamp deltas: min %llu us max %llu us\n", (unsigned long long)dmin,
	    (unsigned long long)dmax);
	check(ok && dmin >= 16600 && dmax <= 33400, "timestamps advance by one or two frames of 16.67 ms");
	/* An event for the vblank after next. */
	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.sequence = 2;
	vbl.request.signal = 0x5151;
	check(drmWaitVBlank(fd, &vbl) == 0, "WAIT_VBLANK with an event");
	n = read_event(fd, 1000, ev, sizeof (ev));
	check(n == 32 && v->base.type == DRM_EVENT_VBLANK && v->user_data == 0x5151 && v->crtc_id == crtc_id &&
	    v->sequence == vbl.reply.sequence, "its VBLANK event with the requested sequence and CRTC ID");
	vbl.request.type = DRM_VBLANK_RELATIVE | _DRM_VBLANK_SIGNAL;
	check(drmWaitVBlank(fd, &vbl) == -1 && errno == EINVAL, "WAIT_VBLANK with _DRM_VBLANK_SIGNAL is EINVAL");
	vbl.request.type = DRM_VBLANK_RELATIVE | (1 << 1);
	vbl.request.sequence = 0;
	check(drmWaitVBlank(fd, &vbl) == -1 && errno == EINVAL, "WAIT_VBLANK on a second CRTC (high bits) is EINVAL");
	/* CRTC_GET_SEQUENCE and CRTC_QUEUE_SEQUENCE. */
	(void) memset(&gs, 0, sizeof (gs));
	gs.crtc_id = crtc_id;
	check(xioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &gs) == 0 && gs.active == 1 && gs.sequence > 0 &&
	    gs.sequence_ns > 0, "CRTC_GET_SEQUENCE: active, sequence and time");
	(void) memset(&qs, 0, sizeof (qs));
	qs.crtc_id = crtc_id;
	qs.flags = DRM_CRTC_SEQUENCE_RELATIVE;
	qs.sequence = 1;
	qs.user_data = 0x7777;
	check(xioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &qs) == 0 && qs.sequence > gs.sequence,
	    "CRTC_QUEUE_SEQUENCE relative 1");
	n = read_event(fd, 1000, ev, sizeof (ev));
	check(n == 32 && sq->base.type == DRM_EVENT_CRTC_SEQUENCE && sq->user_data == 0x7777 &&
	    sq->sequence == qs.sequence && sq->time_ns > 0, "its CRTC_SEQUENCE event");
	qs.flags = 4;
	check(xioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &qs) == EINVAL, "CRTC_QUEUE_SEQUENCE with bad flags is EINVAL");
	gs.crtc_id = 4242;
	check(xioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &gs) == ENOENT, "CRTC_GET_SEQUENCE of an unknown CRTC is ENOENT");
}

static void
test_event_limits(void)
{
	drmVBlank vbl;
	char ev[64];
	struct kms k0, k1;
	int fd = open(CARD, O_RDWR | O_NONBLOCK), i, err = 0, queued = 0;
	ssize_t r;

	check(fd >= 0, "open a second file, non-blocking");
	if (fd < 0)
		return;
	errno = 0;
	r = read(fd, ev, sizeof (ev));
	check(r == -1 && errno == EAGAIN, "read with no events on a non-blocking file is EAGAIN");
	{
		struct pollfd p = { fd, POLLIN, 0 };
		check(poll(&p, 1, 50) == 0, "poll with no events times out");
	}
	(void) kms_stats(&k0);
	for (i = 0; i < 200; i++) {
		(void) memset(&vbl, 0, sizeof (vbl));
		vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
		vbl.request.sequence = 100000;
		if (drmWaitVBlank(fd, &vbl) != 0) {
			err = errno;
			break;
		}
		queued++;
	}
	(void) printf("INFO queued %d far vblank events before errno %d\n", queued, err);
	check(queued == 128 && err == ENOMEM, "the 4096-byte event budget holds 128 events, then ENOMEM");
	(void) kms_stats(&k1);
	check(k1.events_outstanding == k0.events_outstanding + 128, "kstat: 128 events outstanding");
	(void) close(fd);
	(void) kms_stats(&k1);
	check(k1.events_outstanding == k0.events_outstanding && k1.events_discarded >= k0.events_discarded + 128,
	    "closing the file discards its pending events");
	/* A small read buffer gets nothing; a flip event is read whole. */
	fd = open(CARD, O_RDWR);
	(void) memset(&vbl, 0, sizeof (vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.sequence = 1;
	check(drmWaitVBlank(fd, &vbl) == 0, "an event for the next vblank");
	msleep(100);
	check(read(fd, ev, 16) == 0, "read with room for less than one event returns 0 bytes");
	check(read(fd, ev, sizeof (ev)) == 32, "then the whole event");
	(void) close(fd);
}

/* Concurrent readers while the master flips. */
struct worker {
	int iters, errors;
};

static void *
reader(void *arg)
{
	struct worker *w = arg;
	int fd = open(CARD, O_RDWR), i;

	if (fd < 0) {
		w->errors++;
		return (NULL);
	}
	(void) drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	for (i = 0; i < w->iters; i++) {
		drmModeResPtr r = drmModeGetResources(fd);
		drmModeConnectorPtr c = drmModeGetConnectorCurrent(fd, conn_id);
		drmModeCrtcPtr cr = drmModeGetCrtc(fd, crtc_id);
		drmModeObjectPropertiesPtr p = drmModeObjectGetProperties(fd, primary_id, DRM_MODE_OBJECT_PLANE);
		drmVBlank vbl;
		if (r == NULL || c == NULL || cr == NULL || p == NULL)
			w->errors++;
		drmModeFreeResources(r);
		drmModeFreeConnector(c);
		drmModeFreeCrtc(cr);
		drmModeFreeObjectProperties(p);
		(void) memset(&vbl, 0, sizeof (vbl));
		vbl.request.type = DRM_VBLANK_RELATIVE;
		if (drmWaitVBlank(fd, &vbl) != 0)
			w->errors++;
	}
	(void) close(fd);
	return (NULL);
}

static void
test_concurrency(int fd)
{
	pthread_t t[4];
	struct worker w[4];
	struct buf a, b;
	uint32_t fa = 0, fb = 0, cur;
	int i, flips = 0, errs = 0;
	char ev[64];

	(void) buf_create(fd, 1024, 768, 32, &a);
	(void) buf_create(fd, 1024, 768, 32, &b);
	(void) addfb2(fd, 1024, 768, XRGB8888, a.handle, a.pitch, 0, 0, &fa);
	(void) addfb2(fd, 1024, 768, XRGB8888, b.handle, b.pitch, 0, 0, &fb);
	for (i = 0; i < 4; i++) {
		w[i].iters = 100;
		w[i].errors = 0;
		(void) pthread_create(&t[i], NULL, reader, &w[i]);
	}
	cur = fa;
	for (i = 0; i < 60; i++) {
		if (drmModePageFlip(fd, crtc_id, cur, DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0 ||
		    read_event(fd, 1000, ev, sizeof (ev)) != 32)
			errs++;
		else
			flips++;
		cur = cur == fa ? fb : fa;
	}
	for (i = 0; i < 4; i++) {
		(void) pthread_join(t[i], NULL);
		errs += w[i].errors;
	}
	check(flips == 60 && errs == 0, "60 flips with events while four threads query resources and vblanks");
	(void) rmfb(fd, fa);
	(void) rmfb(fd, fb);
	buf_destroy(fd, &a);
	buf_destroy(fd, &b);
}

/*
 * Files opened and closed from many threads, each with a framebuffer, a
 * blob and a pending event at close: a closing file's state must never
 * leak into a new file that is given the same clone id.
 */
static void *
churner(void *arg)
{
	struct worker *w = arg;
	int i;

	for (i = 0; i < w->iters; i++) {
		int fd = open(CARD, O_RDWR);
		struct buf b;
		uint32_t fb = 0, blob = 0;
		drmModeResPtr r;
		drmVBlank vbl;
		char data[8] = "churner";

		if (fd < 0) {
			w->errors++;
			continue;
		}
		r = drmModeGetResources(fd);
		if (r == NULL || r->count_fbs != 0)
			w->errors++;
		drmModeFreeResources(r);
		if (!buf_create(fd, 64, 64, 32, &b) ||
		    addfb2(fd, 64, 64, XRGB8888, b.handle, b.pitch, 0, 0, &fb) != 0 ||
		    drmModeCreatePropertyBlob(fd, data, sizeof (data), &blob) != 0)
			w->errors++;
		r = drmModeGetResources(fd);
		if (r == NULL || r->count_fbs != 1 || r->fbs[0] != fb)
			w->errors++;
		drmModeFreeResources(r);
		(void) memset(&vbl, 0, sizeof (vbl));
		vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
		vbl.request.sequence = 100000;
		(void) drmWaitVBlank(fd, &vbl);
		if (b.map != NULL && b.map != MAP_FAILED)
			(void) munmap((void *)b.map, b.size);
		(void) close(fd);
	}
	return (NULL);
}

static void
test_open_close_churn(void)
{
	pthread_t t[8];
	struct worker w[8];
	int i, errs = 0;

	for (i = 0; i < 8; i++) {
		w[i].iters = 50;
		w[i].errors = 0;
		(void) pthread_create(&t[i], NULL, churner, &w[i]);
	}
	for (i = 0; i < 8; i++) {
		(void) pthread_join(t[i], NULL);
		errs += w[i].errors;
	}
	check(errs == 0, "8 threads x 50 open/addfb/blob/event/close cycles: no state crosses files");
	check(kms_idle("after the churn"), "the churn leaves no framebuffer, blob or event behind");
}

static void
test_close_turns_off(void)
{
	int fd = open(CARD, O_RDWR);
	drmModeCrtcPtr c;
	drmModeResPtr r;
	struct rdrmnull_scanout s;

	c = drmModeGetCrtc(fd, crtc_id);
	check(c != NULL && !c->mode_valid && c->buffer_id == 0, "after the master closed, the CRTC is off");
	drmModeFreeCrtc(c);
	r = drmModeGetResources(fd);
	check(r != NULL && r->count_fbs == 0, "and a new file has no framebuffers");
	drmModeFreeResources(r);
	check(scanout(fd, &s) && !s.active && s.fb_id == 0, "and the scanout shows nothing");
	(void) close(fd);
}

int
main(int argc, char **argv)
{
	int fd, fd2, rfd;
	struct kms k;

	(void) argc;
	(void) argv;
	check(kms_idle("at start"), "kstat rdrmnull:0:kms exists and is idle");
	fd = open(CARD, O_RDWR);
	check(fd >= 0, "open " CARD " (becomes master)");
	fd2 = open(CARD, O_RDWR);
	rfd = open(RENDER, O_RDWR);
	check(fd2 >= 0 && rfd >= 0, "a second primary file and a render file");
	if (fd < 0 || fd2 < 0 || rfd < 0)
		return (failures);
	test_caps(fd);
	test_resources(fd, rfd);
	test_properties(fd);
	test_framebuffers(fd, fd2);
	test_blobs(fd, fd2);
	test_legacy(fd, fd2);
	test_atomic(fd, fd2);
	test_vblank(fd);
	test_event_limits();
	test_concurrency(fd);
	(void) close(rfd);
	(void) close(fd2);
	(void) close(fd);
	test_close_turns_off();
	test_open_close_churn();
	msleep(100);
	check(kms_stats(&k) && k.vblanks > 0 && k.flips > 0 && k.commits > 0, "kstat counted vblanks, flips, commits");
	check(kms_idle("at the end"), "every framebuffer, blob and event is released");
	check(gem_idle(), "every GEM object, handle and mapping is released");
	(void) printf("drm_r3_test: %d failures\n", failures);
	return (failures > 255 ? 255 : failures);
}

#endif	/* PROBE32 */
