/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The R4 test of illumos-drm: the virtio-gpu driver (virtio_gpu) through
 * its ioctls, on a QEMU guest with a virtio-gpu device.
 *
 * Without virgl (-device virtio-gpu-pci) it checks the parameters, dumb
 * buffers as 2D resources, RESOURCE_CREATE of a 2D resource and
 * TRANSFER_TO_HOST, and that the 3D ioctls answer ENOSYS. With virgl
 * (-device virtio-gpu-gl-pci) it drives the host renderer directly: it
 * creates a 3D texture, sends a minimal virgl command stream (create a
 * surface, set the framebuffer, clear) with EXECBUFFER, reads the texture
 * back with TRANSFER_FROM_HOST and checks the pixels; it chains
 * submissions through fence descriptors in the three EXECBUFFER layouts,
 * signals binary and timeline sync objects, shares a texture with a
 * second file through PRIME, creates guest-memory blobs, and checks every
 * refusal (unknown flags, rings without CONTEXT_INIT, bad handles, bad
 * sync object arrays, oversized streams, host-only blobs, sizes of 0) and
 * a fence wait that times out.
 *
 * Usage: drm_r4_test [card [render]] (default $DRM_CARD and $DRM_RENDER,
 * else /dev/dri/card0 and /dev/dri/renderD128; with the device numbers
 * every DRM driver shares, virtio_gpu's nodes are card<N> and
 * renderD<128+N> for the N it was given). Build on illumos:
 *   gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_r4_test \
 *       drm_r4_test.c -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lpthread
 * where drm/ holds the headers of tools/uapi/generate.sh (libdrm 2.4.109,
 * patched, with virtgpu_drm.h of Linux v6.12).
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
#include <sys/stat.h>
#include <sys/types.h>

#include "drm.h"
#include "drm_mode.h"
#include "virtgpu_drm.h"
#include <xf86drm.h>

/* The nodes under test (see the usage above). */
static const char *card_node = "/dev/dri/card0";
static const char *render_node = "/dev/dri/renderD128";

/* The older layouts (crates/drm-uapi/src/virtgpu_compat.rs). */
struct execbuffer_v1 {
	uint32_t flags;
	uint32_t size;
	uint64_t command;
	uint64_t bo_handles;
	uint32_t num_bo_handles;
	int32_t fence_fd;
};
struct execbuffer_v2 {
	uint32_t flags;
	uint32_t size;
	uint64_t command;
	uint64_t bo_handles;
	uint32_t num_bo_handles;
	int32_t fence_fd;
	uint32_t ring_idx;
	uint32_t pad;
};
struct transfer_v1 {
	uint32_t bo_handle;
	struct drm_virtgpu_3d_box box;
	uint32_t level;
	uint32_t offset;
};
_Static_assert(sizeof (struct execbuffer_v1) == 32, "layout");
_Static_assert(sizeof (struct execbuffer_v2) == 40, "layout");
_Static_assert(sizeof (struct drm_virtgpu_execbuffer) == 64, "layout");
_Static_assert(sizeof (struct transfer_v1) == 36, "layout");
#define	EXECBUFFER_V1 DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER, struct execbuffer_v1)
#define	EXECBUFFER_V2 DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER, struct execbuffer_v2)
#define	TRANSFER_FROM_HOST_V1 DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_FROM_HOST, struct transfer_v1)

/* virglrenderer's virgl_protocol.h. */
#define	VIRGL_CMD0(cmd, obj, len)	((cmd) | ((obj) << 8) | ((len) << 16))
#define	VIRGL_CCMD_CREATE_OBJECT	1
#define	VIRGL_CCMD_DESTROY_OBJECT	3
#define	VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define	VIRGL_CCMD_CLEAR		7
#define	VIRGL_OBJECT_SURFACE		8
#define	VIRGL_FORMAT_B8G8R8X8_UNORM	2
#define	VIRGL_BIND_RENDER_TARGET	(1 << 1)
#define	VIRGL_BIND_SAMPLER_VIEW		(1 << 3)
#define	PIPE_TEXTURE_2D			2
#define	PIPE_CLEAR_COLOR0		(1 << 2)

#define	W	64
#define	H	64

static int passes, failures;
static int fd;

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

/* ioctl(2), retried on EINTR; returns 0 or errno. */
static int
xioctl(int f, unsigned long cmd, void *arg)
{
	int r;

	do {
		r = ioctl(f, cmd, arg);
	} while (r == -1 && errno == EINTR);
	return (r == -1 ? errno : 0);
}

static uint64_t
getparam(int f, uint64_t param, int *err)
{
	uint64_t v = 0;
	struct drm_virtgpu_getparam g = { .param = param, .value = (uintptr_t)&v };

	*err = xioctl(f, DRM_IOCTL_VIRTGPU_GETPARAM, &g);
	return (v);
}

static uint32_t
fbits(float x)
{
	uint32_t u;

	(void) memcpy(&u, &x, 4);
	return (u);
}

/*
 * A virgl command stream that clears resource `res` (a B8G8R8X8 texture)
 * to (r, g, b) through surface object `surf`: create the surface, bind it
 * as the only color buffer, clear, destroy the surface.
 */
static size_t
clear_stream(uint32_t *s, uint32_t surf, uint32_t res, float r, float g, float b)
{
	size_t n = 0;

	s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
	s[n++] = surf;
	s[n++] = res;
	s[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
	s[n++] = 0;		/* level */
	s[n++] = 0;		/* first and last layer */
	s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
	s[n++] = 1;		/* one color buffer */
	s[n++] = 0;		/* no depth/stencil surface */
	s[n++] = surf;
	s[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
	s[n++] = PIPE_CLEAR_COLOR0;
	s[n++] = fbits(r);
	s[n++] = fbits(g);
	s[n++] = fbits(b);
	s[n++] = fbits(1.0f);
	s[n++] = 0;		/* depth, a double */
	s[n++] = 0;
	s[n++] = 0;		/* stencil */
	s[n++] = VIRGL_CMD0(VIRGL_CCMD_DESTROY_OBJECT, VIRGL_OBJECT_SURFACE, 1);
	s[n++] = surf;
	return (n * 4);
}

struct tex {
	uint32_t bo, res;
	uint8_t *map;
};

static int
tex_create(int f, struct tex *t)
{
	struct drm_virtgpu_resource_create rc = {
		.target = PIPE_TEXTURE_2D,
		.format = VIRGL_FORMAT_B8G8R8X8_UNORM,
		.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
		.width = W, .height = H, .depth = 1, .array_size = 1,
		.last_level = 0, .nr_samples = 0, .flags = 0,
		.size = W * H * 4, .stride = W * 4,
	};
	struct drm_virtgpu_map m = { 0 };
	int e;

	if ((e = xioctl(f, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc)) != 0)
		return (e);
	t->bo = rc.bo_handle;
	t->res = rc.res_handle;
	m.handle = t->bo;
	if ((e = xioctl(f, DRM_IOCTL_VIRTGPU_MAP, &m)) != 0)
		return (e);
	t->map = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, f, (off_t)m.offset);
	return (t->map == MAP_FAILED ? errno : 0);
}

static int
from_host(int f, struct tex *t)
{
	struct drm_virtgpu_3d_transfer_from_host x = {
		.bo_handle = t->bo,
		.box = { .x = 0, .y = 0, .z = 0, .w = W, .h = H, .d = 1 },
		.level = 0, .offset = 0, .stride = W * 4, .layer_stride = 0,
	};
	struct drm_virtgpu_3d_wait w = { .handle = t->bo };
	int e;

	if ((e = xioctl(f, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &x)) != 0)
		return (e);
	return (xioctl(f, DRM_IOCTL_VIRTGPU_WAIT, &w));
}

/* Whether every pixel is about (b, g, r) in B8G8R8X8. */
static int
pixels_are(struct tex *t, int b, int g, int r)
{
	int bad = 0;

	for (int i = 0; i < W * H; i++) {
		uint8_t *p = t->map + 4 * i;

		if (abs(p[0] - b) > 2 || abs(p[1] - g) > 2 || abs(p[2] - r) > 2)
			bad++;
	}
	if (bad != 0)
		(void) printf("  %d of %d pixels differ; first pixel %02x %02x %02x %02x\n",
		    bad, W * H, t->map[0], t->map[1], t->map[2], t->map[3]);
	return (bad == 0);
}

static int
exec64(int f, uint32_t flags, const void *cmd, uint32_t size, uint32_t *bos, uint32_t nbos,
    int *fence_fd, struct drm_virtgpu_execbuffer_syncobj *in, uint32_t nin,
    struct drm_virtgpu_execbuffer_syncobj *out, uint32_t nout, uint32_t ring)
{
	struct drm_virtgpu_execbuffer x = {
		.flags = flags, .size = size, .command = (uintptr_t)cmd,
		.bo_handles = (uintptr_t)bos, .num_bo_handles = nbos,
		.fence_fd = fence_fd != NULL ? *fence_fd : -1,
		.ring_idx = ring,
		.syncobj_stride = sizeof (struct drm_virtgpu_execbuffer_syncobj),
		.num_in_syncobjs = nin, .num_out_syncobjs = nout,
		.in_syncobjs = (uintptr_t)in, .out_syncobjs = (uintptr_t)out,
	};
	int e = xioctl(f, DRM_IOCTL_VIRTGPU_EXECBUFFER, &x);

	if (e == 0 && fence_fd != NULL)
		*fence_fd = x.fence_fd;
	return (e);
}

static int
fence_signaled(int ffd, int ms)
{
	struct pollfd p = { .fd = ffd, .events = POLLIN };

	return (poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0);
}

static int64_t
now_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
}

static void
params(int *has3d, uint64_t *mask, int *blob, int *cinit)
{
	int e;
	uint64_t v;

	*has3d = (int)getparam(fd, VIRTGPU_PARAM_3D_FEATURES, &e);
	check(e == 0, "GETPARAM 3D_FEATURES");
	v = getparam(fd, VIRTGPU_PARAM_CAPSET_QUERY_FIX, &e);
	check(e == 0 && v == 1, "GETPARAM CAPSET_QUERY_FIX is 1");
	*blob = (int)getparam(fd, VIRTGPU_PARAM_RESOURCE_BLOB, &e);
	check(e == 0, "GETPARAM RESOURCE_BLOB");
	v = getparam(fd, VIRTGPU_PARAM_HOST_VISIBLE, &e);
	check(e == 0 && v == 0, "GETPARAM HOST_VISIBLE is 0 (no shared memory regions)");
	v = getparam(fd, VIRTGPU_PARAM_CROSS_DEVICE, &e);
	check(e == 0 && v == 0, "GETPARAM CROSS_DEVICE is 0");
	*cinit = (int)getparam(fd, VIRTGPU_PARAM_CONTEXT_INIT, &e);
	check(e == 0, "GETPARAM CONTEXT_INIT");
	*mask = getparam(fd, VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, &e);
	check(e == 0, "GETPARAM SUPPORTED_CAPSET_IDs");
	v = getparam(fd, VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME, &e);
	check(e == 0 && v == (uint64_t)*cinit, "GETPARAM EXPLICIT_DEBUG_NAME follows CONTEXT_INIT");
	(void) getparam(fd, 99, &e);
	check(e == EINVAL, "GETPARAM of an unknown parameter is EINVAL");
	(void) printf("3d %d, blob %d, context_init %d, capsets %#llx\n", *has3d, *blob, *cinit,
	    (unsigned long long)*mask);
}

static void
caps(uint64_t mask)
{
	uint8_t buf[4096];
	struct drm_virtgpu_get_caps g = { 0 };
	int e;

	if (mask == 0) {
		g.cap_set_id = 1;
		g.size = sizeof (buf);
		g.addr = (uintptr_t)buf;
		e = xioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &g);
		check(e == ENOSYS, "GET_CAPS without capability sets is ENOSYS");
		return;
	}
	for (uint32_t id = 1; id <= 2; id++) {
		if ((mask & (1ull << id)) == 0)
			continue;
		(void) memset(buf, 0xa5, sizeof (buf));
		g.cap_set_id = id;
		g.cap_set_ver = 1;
		g.size = sizeof (buf);
		g.addr = (uintptr_t)buf;
		e = xioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &g);
		(void) printf("capset %u: first words %08x %08x %08x\n", id,
		    ((uint32_t *)buf)[0], ((uint32_t *)buf)[1], ((uint32_t *)buf)[2]);
		check(e == 0 && ((uint32_t *)buf)[0] != 0xa5a5a5a5u,
		    id == 1 ? "GET_CAPS virgl (capset 1) copies the host's caps" :
		    "GET_CAPS virgl2 (capset 2) copies the host's caps");
	}
	g.cap_set_id = (mask & 4) ? 2 : 1;
	g.size = 0;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &g) == EINVAL, "GET_CAPS with size 0 is EINVAL");
	g.size = sizeof (buf);
	g.cap_set_id = 63;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &g) == EINVAL, "GET_CAPS of an unknown capability set is EINVAL");
	g.cap_set_id = (mask & 4) ? 2 : 1;
	g.cap_set_ver = 1000;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &g) == EINVAL, "GET_CAPS of a too new version is EINVAL");
	g.cap_set_ver = 1;
	g.addr = 8;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &g) == EFAULT, "GET_CAPS to a bad address is EFAULT");
}

static void
dumb(void)
{
	struct drm_mode_create_dumb c = { .width = 640, .height = 480, .bpp = 32 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_virtgpu_resource_info ri = { 0 };
	struct drm_virtgpu_map m = { 0 };
	struct drm_gem_close gc = { 0 };
	int e;

	e = xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c);
	check(e == 0 && c.pitch == 640 * 4 && c.size >= 640 * 480 * 4,
	    "MODE_CREATE_DUMB 640x480x32: pitch 2560, a 2D resource");
	ri.bo_handle = c.handle;
	e = xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ri);
	check(e == 0 && ri.res_handle != 0 && ri.size == c.size && ri.blob_mem == 0,
	    "RESOURCE_INFO of the dumb buffer: resource, size, not a blob");
	md.handle = c.handle;
	m.handle = c.handle;
	e = xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
	check(e == 0 && xioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &m) == 0 && m.offset == md.offset,
	    "VIRTGPU_MAP gives the MAP_DUMB offset");
	void *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	check(p != MAP_FAILED && ((uint8_t *)p)[0] == 0, "the dumb buffer maps and reads as zeros");
	if (p != MAP_FAILED)
		(void) munmap(p, c.size);
	gc.handle = c.handle;
	check(xioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == 0, "GEM_CLOSE of the dumb buffer");
	c.bpp = 16;
	check(xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) == EINVAL, "MODE_CREATE_DUMB with 16 bpp is EINVAL");
	m.handle = 4242;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &m) == ENOENT, "VIRTGPU_MAP of an unknown handle is ENOENT");
	ri.bo_handle = 4242;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ri) == ENOENT, "RESOURCE_INFO of an unknown handle is ENOENT");
}

static void
two_d(void)
{
	struct drm_virtgpu_resource_create rc = {
		.target = 2, .format = 2, .bind = 0, .width = W, .height = H, .depth = 1,
		.array_size = 1, .size = W * H * 4,
	};
	struct drm_virtgpu_3d_transfer_to_host t = { 0 };
	struct drm_virtgpu_3d_transfer_from_host f = { 0 };
	struct drm_virtgpu_execbuffer x = { 0 };
	int e = xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc);

	check(e == 0 && rc.res_handle != 0 && rc.bo_handle != 0, "RESOURCE_CREATE of a 2D resource");
	rc.depth = 2;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc) == EINVAL, "2D RESOURCE_CREATE with depth 2 is EINVAL");
	rc.depth = 1;
	rc.target = 3;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc) == EINVAL, "2D RESOURCE_CREATE of a 3D target is EINVAL");
	t.bo_handle = rc.bo_handle;
	t.box.w = W;
	t.box.h = H;
	t.box.d = 1;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &t) == 0, "2D TRANSFER_TO_HOST");
	f.bo_handle = rc.bo_handle;
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &f) == ENOSYS, "2D TRANSFER_FROM_HOST is ENOSYS");
	check(xioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &x) == ENOSYS, "EXECBUFFER without virgl is ENOSYS");
}

static void
three_d(int blob, int cinit)
{
	uint32_t s[64];
	struct tex t, t2;
	int e, ffd = -1, ffd2 = -1;
	size_t n;

	e = tex_create(fd, &t);
	check(e == 0, "RESOURCE_CREATE of a 64x64 B8G8R8X8 texture, mapped");
	if (e != 0)
		return;
	(void) memset(t.map, 0x11, W * H * 4);

	/* Clear to (1.0, 0.5, 0.25) and read it back. */
	n = clear_stream(s, 1, t.res, 1.0f, 0.5f, 0.25f);
	e = exec64(fd, VIRTGPU_EXECBUF_FENCE_FD_OUT, s, (uint32_t)n, &t.bo, 1, &ffd, NULL, 0, NULL, 0, 0);
	check(e == 0 && ffd >= 0, "EXECBUFFER (64-byte layout) with a virgl clear returns an out-fence");
	check(ffd >= 0 && fence_signaled(ffd, 5000), "the out-fence signals (poll(2) of the sync file)");
	e = from_host(fd, &t);
	check(e == 0, "TRANSFER_FROM_HOST and WAIT");
	check(pixels_are(&t, 0x40, 0x80, 0xff), "the texture reads back as the clear color (B 40, G 80, R ff)");

	/* The 40-byte layout of Mesa 21.3, waiting on the first fence. */
	n = clear_stream(s, 2, t.res, 0.0f, 1.0f, 0.0f);
	{
		struct execbuffer_v2 x = {
			.flags = VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT,
			.size = (uint32_t)n, .command = (uintptr_t)s, .bo_handles = (uintptr_t)&t.bo,
			.num_bo_handles = 1, .fence_fd = ffd,
		};
		e = xioctl(fd, EXECBUFFER_V2, &x);
		ffd2 = x.fence_fd;
		check(e == 0 && ffd2 >= 0 && ffd2 != ffd,
		    "EXECBUFFER (40-byte layout of Mesa 21.3) with an in-fence and an out-fence");
	}
	{
		struct transfer_v1 x = { .bo_handle = t.bo, .box = { .w = W, .h = H, .d = 1 } };
		struct drm_virtgpu_3d_wait w = { .handle = t.bo };

		e = xioctl(fd, TRANSFER_FROM_HOST_V1, &x);
		check(e == 0 && xioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == 0,
		    "TRANSFER_FROM_HOST (36-byte layout of libdrm 2.4.109) and WAIT");
		check(pixels_are(&t, 0x00, 0xff, 0x00), "the texture reads back green");
	}
	check(fence_signaled(ffd2, 5000), "the second out-fence signals");

	/* The 32-byte layout of libdrm 2.4.109. */
	n = clear_stream(s, 3, t.res, 0.0f, 0.0f, 1.0f);
	{
		struct execbuffer_v1 x = {
			.flags = 0, .size = (uint32_t)n, .command = (uintptr_t)s,
			.bo_handles = (uintptr_t)&t.bo, .num_bo_handles = 1, .fence_fd = -1,
		};
		e = xioctl(fd, EXECBUFFER_V1, &x);
		check(e == 0, "EXECBUFFER (32-byte layout of libdrm 2.4.109)");
		check(from_host(fd, &t) == 0 && pixels_are(&t, 0xff, 0x00, 0x00), "the texture reads back blue");
	}

	/* Sync objects: a binary out, a timeline out, an in with reset. */
	{
		uint32_t bin, tl;
		struct drm_virtgpu_execbuffer_syncobj out[2], in;
		uint64_t point = 3;

		check(drmSyncobjCreate(fd, 0, &bin) == 0 && drmSyncobjCreate(fd, 0, &tl) == 0, "two sync objects");
		out[0] = (struct drm_virtgpu_execbuffer_syncobj){ .handle = bin, .flags = 0, .point = 0 };
		out[1] = (struct drm_virtgpu_execbuffer_syncobj){ .handle = tl, .flags = 0, .point = 3 };
		n = clear_stream(s, 4, t.res, 1.0f, 1.0f, 1.0f);
		e = exec64(fd, 0, s, (uint32_t)n, &t.bo, 1, NULL, NULL, 0, out, 2, 0);
		check(e == 0, "EXECBUFFER signalling a binary sync object and timeline point 3");
		check(drmSyncobjWait(fd, &bin, 1, now_ns() + 5000000000LL, 0, NULL) == 0,
		    "the binary sync object signals");
		check(drmSyncobjTimelineWait(fd, &tl, &point, 1, now_ns() + 5000000000LL, 0, NULL) == 0,
		    "timeline point 3 signals");
		/* A fence wait that times out: point 4 is never submitted. */
		point = 4;
		int64_t t0 = now_ns();
		e = drmSyncobjTimelineWait(fd, &tl, &point, 1, now_ns() + 100000000LL,
		    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
		int64_t dt = now_ns() - t0;
		check(e != 0 && errno == ETIME && dt >= 90000000LL && dt < 2000000000LL,
		    "waiting for timeline point 4, never submitted, times out with ETIME after about 100 ms");
		in = (struct drm_virtgpu_execbuffer_syncobj){ .handle = bin, .flags = VIRTGPU_EXECBUF_SYNCOBJ_RESET };
		n = clear_stream(s, 5, t.res, 0.0f, 0.0f, 0.0f);
		e = exec64(fd, 0, s, (uint32_t)n, &t.bo, 1, NULL, &in, 1, NULL, 0, 0);
		check(e == 0, "EXECBUFFER waiting on a sync object with VIRTGPU_EXECBUF_SYNCOBJ_RESET");
		e = drmSyncobjWait(fd, &bin, 1, 0, 0, NULL);
		check(e != 0, "the reset sync object has no fence afterwards");
		in.flags = 0;
		e = exec64(fd, 0, s, (uint32_t)n, &t.bo, 1, NULL, &in, 1, NULL, 0, 0);
		check(e == EINVAL, "an in sync object without a fence is EINVAL");
		(void) drmSyncobjDestroy(fd, bin);
		(void) drmSyncobjDestroy(fd, tl);
	}

	/* Refusals. */
	n = clear_stream(s, 6, t.res, 0.0f, 0.0f, 0.0f);
	check(exec64(fd, 0x100, s, (uint32_t)n, &t.bo, 1, NULL, NULL, 0, NULL, 0, 0) == EINVAL,
	    "EXECBUFFER with an unknown flag is EINVAL");
	check(exec64(fd, VIRTGPU_EXECBUF_RING_IDX, s, (uint32_t)n, &t.bo, 1, NULL, NULL, 0, NULL, 0, 0) == EINVAL,
	    "EXECBUFFER on ring 0 of a context without rings is EINVAL");
	{
		uint32_t bad = 4242;
		check(exec64(fd, 0, s, (uint32_t)n, &bad, 1, NULL, NULL, 0, NULL, 0, 0) == ENOENT,
		    "EXECBUFFER with an unknown buffer handle is ENOENT");
	}
	{
		struct drm_virtgpu_execbuffer_syncobj in = { .handle = 4242 };
		struct drm_virtgpu_execbuffer x = {
			.size = (uint32_t)n, .command = (uintptr_t)s, .syncobj_stride = 8,
			.num_in_syncobjs = 1, .in_syncobjs = (uintptr_t)&in, .fence_fd = -1,
		};
		check(xioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &x) == EINVAL, "EXECBUFFER with a wrong syncobj_stride is EINVAL");
		x.syncobj_stride = sizeof (in);
		in.flags = 2;
		check(xioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &x) == EINVAL, "EXECBUFFER with an unknown sync object flag is EINVAL");
		in.flags = 0;
		check(xioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &x) == ENOENT, "EXECBUFFER with an unknown sync object is ENOENT");
	}
	{
		int nul = open("/dev/null", O_RDONLY);
		int f = nul;

		check(exec64(fd, VIRTGPU_EXECBUF_FENCE_FD_IN, s, (uint32_t)n, &t.bo, 1, &f, NULL, 0, NULL, 0, 0) == EINVAL,
		    "EXECBUFFER with an in-fence that is not a sync file is EINVAL");
		(void) close(nul);
	}
	check(exec64(fd, 0, s, 2u << 20, &t.bo, 1, NULL, NULL, 0, NULL, 0, 0) == EINVAL,
	    "EXECBUFFER with a 2 MiB command stream is EINVAL");
	check(exec64(fd, 0, (void *)8, 64, &t.bo, 1, NULL, NULL, 0, NULL, 0, 0) == EFAULT,
	    "EXECBUFFER with an unreadable command stream is EFAULT");
	{
		struct drm_virtgpu_3d_wait w = { .handle = 4242 };
		check(xioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == ENOENT, "WAIT on an unknown handle is ENOENT");
	}
	/* NOWAIT right after a submission may find it busy; either is fine. */
	{
		struct drm_virtgpu_3d_wait w = { .handle = t.bo, .flags = VIRTGPU_WAIT_NOWAIT };
		n = clear_stream(s, 7, t.res, 0.5f, 0.5f, 0.5f);
		(void) exec64(fd, 0, s, (uint32_t)n, &t.bo, 1, NULL, NULL, 0, NULL, 0, 0);
		e = xioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &w);
		(void) printf("WAIT NOWAIT right after EXECBUFFER: %s\n", e == 0 ? "idle" : strerror(e));
		check(e == 0 || e == EBUSY, "WAIT NOWAIT answers 0 or EBUSY");
		w.flags = 0;
		check(xioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == 0, "a blocking WAIT returns once the work is done");
		w.flags = VIRTGPU_WAIT_NOWAIT;
		check(xioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == 0, "WAIT NOWAIT afterwards finds it idle");
	}

	/* PRIME: a second file (the render node) imports the texture. */
	{
		int prime = -1, r = open(render_node, O_RDWR);
		uint32_t h2 = 0;

		check(r >= 0 && drmPrimeHandleToFD(fd, t.bo, DRM_CLOEXEC, &prime) == 0 &&
		    drmPrimeFDToHandle(r, prime, &h2) == 0, "PRIME export and import on the render node");
		t2 = t;
		t2.bo = h2;
		n = clear_stream(s, 1, t.res, 0.0f, 0.5f, 1.0f);
		e = exec64(r, 0, s, (uint32_t)n, &h2, 1, NULL, NULL, 0, NULL, 0, 0);
		check(e == 0 && from_host(r, &t2) == 0 && pixels_are(&t, 0xff, 0x80, 0x00),
		    "the render file's context uses the imported resource (cleared, read back)");
		if (prime >= 0)
			(void) close(prime);
		if (r >= 0)
			(void) close(r);
	}

	/* Many requests: resources created and freed, and a burst of
	 * submissions deeper than the control queue. */
	{
		int ok = 1, sub = 0;
		for (int i = 0; i < 200 && ok; i++) {
			struct tex x;
			struct drm_gem_close gc = { 0 };
			ok = tex_create(fd, &x) == 0;
			if (ok) {
				(void) munmap(x.map, W * H * 4);
				gc.handle = x.bo;
				ok = xioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == 0;
			}
		}
		check(ok, "200 textures created, mapped and closed");
		n = clear_stream(s, 8, t.res, 0.25f, 0.25f, 0.25f);
		for (int i = 0; i < 2000; i++)
			if (exec64(fd, 0, s, (uint32_t)n, &t.bo, 1, NULL, NULL, 0, NULL, 0, 0) == 0)
				sub++;
		check(sub == 2000 && from_host(fd, &t) == 0 && pixels_are(&t, 0x40, 0x40, 0x40),
		    "2000 submissions in a row, then the texture reads back grey");
	}

	/* Blobs with guest memory. */
	{
		struct drm_virtgpu_resource_create_blob b = {
			.blob_mem = VIRTGPU_BLOB_MEM_GUEST, .blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
			.size = 65536,
		};
		struct drm_virtgpu_resource_info ri = { 0 };
		e = xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b);
		if (!blob) {
			check(e == EINVAL, "RESOURCE_CREATE_BLOB without the blob feature is EINVAL");
		} else {
			ri.bo_handle = b.bo_handle;
			check(e == 0 && xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ri) == 0 &&
			    ri.blob_mem == VIRTGPU_BLOB_MEM_GUEST && ri.size == 65536,
			    "RESOURCE_CREATE_BLOB of 64 KiB guest memory; RESOURCE_INFO says BLOB_MEM_GUEST");
			b.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
			check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b) == EINVAL,
			    "a host-memory blob (HOST_VISIBLE 0) is EINVAL");
			b.blob_mem = 0;
			check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b) == EINVAL, "blob_mem 0 is EINVAL");
			b.blob_mem = VIRTGPU_BLOB_MEM_GUEST;
			b.blob_id = 1;
			check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b) == EINVAL,
			    "a guest blob with a blob_id is EINVAL");
			b.blob_id = 0;
			b.blob_flags = VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE;
			check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b) == EINVAL,
			    "a cross-device blob (CROSS_DEVICE 0) is EINVAL");
			b.blob_flags = 0;
			b.size = 0;
			check(xioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b) == EINVAL, "a blob of size 0 is EINVAL");
		}
	}

	/* CONTEXT_INIT on a fresh file. */
	{
		int f2 = open(render_node, O_RDWR);
		struct drm_virtgpu_context_set_param p[2] = {
			{ .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID, .value = 2 },
			{ .param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS, .value = 2 },
		};
		struct drm_virtgpu_context_init ci = { .num_params = 2, .ctx_set_params = (uintptr_t)p };

		e = xioctl(f2, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci);
		if (!cinit) {
			check(e == EINVAL, "CONTEXT_INIT without the context-init feature is EINVAL");
		} else {
			check(e == 0, "CONTEXT_INIT with capset virgl2 and two rings");
			check(xioctl(f2, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci) == EEXIST, "a second CONTEXT_INIT is EEXIST");
			n = clear_stream(s, 1, t.res, 0.0f, 0.0f, 0.0f);
			check(exec64(f2, VIRTGPU_EXECBUF_RING_IDX, s, (uint32_t)n, NULL, 0, NULL, NULL, 0, NULL, 0, 1) == 0,
			    "EXECBUFFER on ring 1");
			check(exec64(f2, VIRTGPU_EXECBUF_RING_IDX, s, (uint32_t)n, NULL, 0, NULL, NULL, 0, NULL, 0, 2) == EINVAL,
			    "EXECBUFFER on ring 2 of two is EINVAL");
		}
		(void) close(f2);
	}
	if (ffd >= 0)
		(void) close(ffd);
	if (ffd2 >= 0)
		(void) close(ffd2);
	(void) munmap(t.map, W * H * 4);
}

/*
 * Eight threads at once, each 100 rounds on a file of its own (the even
 * ones on the render node) or on the shared card file: create a texture,
 * clear it with a fenced EXECBUFFER, read it back, check it, close it.
 * Fence ids, the control queue, completions on the task queue and on
 * waiting threads, and resource creation and destruction all race.
 */
#define	THREADS	8
#define	ROUNDS	100

struct worker {
	int id, shared, bad;
};

static void *
stress_thread(void *arg)
{
	struct worker *w = arg;
	int f = w->shared ? fd : open((w->id % 2) ? card_node : render_node, O_RDWR);
	uint32_t s[64];

	if (f < 0) {
		w->bad++;
		return (NULL);
	}
	for (int i = 0; i < ROUNDS; i++) {
		struct tex t;
		struct drm_gem_close gc = { 0 };
		int ffd = -1;
		float c = (float)((w->id * 31 + i) % 256) / 255.0f;
		int v = (w->id * 31 + i) % 256;

		if (tex_create(f, &t) != 0) {
			w->bad++;
			continue;
		}
		size_t n = clear_stream(s, 100 + w->id, t.res, c, c, c);
		if (exec64(f, VIRTGPU_EXECBUF_FENCE_FD_OUT, s, (uint32_t)n, &t.bo, 1, &ffd, NULL, 0, NULL, 0, 0) != 0 ||
		    !fence_signaled(ffd, 10000) || from_host(f, &t) != 0) {
			w->bad++;
		} else {
			uint8_t *p = t.map + 4 * ((i * 37) % (W * H));
			if (abs(p[0] - v) > 2 || abs(p[1] - v) > 2 || abs(p[2] - v) > 2)
				w->bad++;
		}
		if (ffd >= 0)
			(void) close(ffd);
		(void) munmap(t.map, W * H * 4);
		gc.handle = t.bo;
		if (xioctl(f, DRM_IOCTL_GEM_CLOSE, &gc) != 0)
			w->bad++;
	}
	if (!w->shared)
		(void) close(f);
	return (NULL);
}

static void
stress(void)
{
	pthread_t th[THREADS];
	struct worker w[THREADS];
	int bad = 0;

	for (int i = 0; i < THREADS; i++) {
		w[i] = (struct worker){ .id = i, .shared = i >= 6, .bad = 0 };
		(void) pthread_create(&th[i], NULL, stress_thread, &w[i]);
	}
	for (int i = 0; i < THREADS; i++) {
		(void) pthread_join(th[i], NULL);
		bad += w[i].bad;
	}
	(void) printf("stress: %d threads x %d rounds, %d failures\n", THREADS, ROUNDS, bad);
	check(bad == 0, "8 threads (6 files of their own, 2 on a shared file) create, clear, fence, read back and close 800 textures");
}

/* Waits up to a minute for `path` to exist. */
static void
wait_for(const char *path)
{
	for (int i = 0; i < 600 && access(path, F_OK) != 0; i++)
		(void) usleep(100000);
}

/*
 * `drm_r4_test hold`: keeps the device busy for run-r4-test.sh's refused
 * detach: a file with a dumb buffer and its dma-buf until hold.next
 * exists, then only the dma-buf until hold.release exists.
 */
static int
hold(void)
{
	struct drm_mode_create_dumb c = { .width = 64, .height = 64, .bpp = 32 };
	int prime = -1;

	if ((fd = open(card_node, O_RDWR)) < 0 || xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0 ||
	    drmPrimeHandleToFD(fd, c.handle, DRM_CLOEXEC, &prime) != 0) {
		perror("hold");
		return (1);
	}
	(void) printf("hold: a file, a dumb buffer and its dma-buf (fd %d)\n", prime);
	(void) fflush(stdout);
	wait_for("hold.next");
	(void) close(fd);
	(void) printf("hold: only the dma-buf\n");
	(void) fflush(stdout);
	wait_for("hold.release");
	(void) close(prime);
	(void) printf("hold: released\n");
	return (0);
}

int
main(int argc, char **argv)
{
	const char *card;

	if (getenv("DRM_CARD") != NULL && *getenv("DRM_CARD") != '\0')
		card_node = getenv("DRM_CARD");
	if (getenv("DRM_RENDER") != NULL && *getenv("DRM_RENDER") != '\0')
		render_node = getenv("DRM_RENDER");
	if (argc > 1 && strcmp(argv[1], "hold") == 0)
		return (hold());
	if (argc > 1)
		card_node = argv[1];
	if (argc > 2)
		render_node = argv[2];
	card = card_node;
	(void) printf("nodes under test: %s %s\n", card_node, render_node);
	int has3d, blob, cinit;
	uint64_t mask;
	drmVersionPtr v;

	if ((fd = open(card, O_RDWR)) < 0) {
		perror(card);
		return (1);
	}
	v = drmGetVersion(fd);
	check(v != NULL && strcmp(v->name, "virtio_gpu") == 0 && v->version_major == 0 && v->version_minor == 1,
	    "VERSION: virtio_gpu 0.1 (Mesa's virgl winsys wants major 0)");
	if (v != NULL) {
		(void) printf("driver %s %d.%d.%d, %s\n", v->name, v->version_major, v->version_minor,
		    v->version_patchlevel, v->desc);
		drmFreeVersion(v);
	}
	params(&has3d, &mask, &blob, &cinit);
	caps(mask);
	dumb();
	if (has3d) {
		three_d(blob, cinit);
		stress();
	} else {
		two_d();
	}
	(void) close(fd);
	(void) printf("drm_r4_test: %d passed, %d failed\n", passes, failures);
	return (failures == 0 ? 0 : 1);
}
