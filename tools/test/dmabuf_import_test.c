/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Provenance: written for work item G3 (Mesa 25.3.6 in oi-userland,
 * 2026-10-03) as artifacts/g3-mesa/guest/dmabuf_import_test.c in the
 * owner's workspace, where it reproduced the DRI3 blank screen of G4 at
 * the EGL/GBM level and, with DTrace, led to the G8 fix (a re-import of an
 * object a file holds detached the virtio_gpu resource from the file's
 * context). Added to illumos-drm unchanged apart from this header and
 * DRM_RENDER as the default node.
 *
 * Cross-process dma-buf import through Mesa (the path Xorg glamor's DRI3
 * PixmapFromBuffer uses), on a virtio_gpu render node with virgl.
 *
 * The exporter process renders a known colour into a GBM buffer through EGL
 * and passes its dma-buf fd over a socket.  The importer process opens its
 * own render node and GBM/EGL display and, as its first GL work (no drawing
 * before), imports the fd (a) as an EGL_LINUX_DMA_BUF_EXT image and (b) via
 * gbm_bo_import(GBM_BO_IMPORT_FD) + EGL_NATIVE_PIXMAP_KHR, binds each to a
 * texture and reads it back through an FBO.  Then the importer draws into
 * the imported buffer and the exporter reads its own buffer back.  Import
 * (b) is the second import of the same buffer into the importer's file:
 * before G8 it detached the resource from the importer's context, and the
 * importer's drawing failed (host: "Illegal resource").  SKIP_A=1 imports
 * once; IMPORTER_DRAW_FIRST=1 draws into a private buffer first.
 *
 * Needs a Mesa with dma-buf import on illumos (G3's Mesa 25.3.6 patch
 * 08-illumos-dmabuf).
 *
 *	gcc -m64 -Wall -D_XOPEN_SOURCE=600 -D__EXTENSIONS__ -o dmabuf_import_test \
 *	    dmabuf_import_test.c -L/usr/lib/amd64 -R/usr/lib/amd64 -lEGL -lGL -lgbm -lsocket
 *	./dmabuf_import_test [render node]	(default $DRM_RENDER, else /dev/dri/renderD128)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#include <drm/drm_fourcc.h>

#define W 64
#define H 64
static const char *node = "/dev/dri/renderD128";
static int failures;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { failures++; printf("FAIL " __VA_ARGS__); } printf("\n"); fflush(stdout); } while (0)

static PFNEGLCREATEIMAGEKHRPROC createImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture;

struct ctx { int fd; struct gbm_device *gbm; EGLDisplay dpy; EGLContext c; };

static int
setup(struct ctx *x, const char *who)
{
	x->fd = open(node, O_RDWR | O_CLOEXEC);
	x->gbm = gbm_create_device(x->fd);
	x->dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, x->gbm, NULL);
	EGLint maj, min;
	if (x->fd < 0 || !x->gbm || !eglInitialize(x->dpy, &maj, &min)) {
		printf("FAIL %s: GBM/EGL on %s\n", who, node);
		return -1;
	}
	eglBindAPI(EGL_OPENGL_API);
	x->c = eglCreateContext(x->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
	if (x->c == EGL_NO_CONTEXT || !eglMakeCurrent(x->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, x->c)) {
		printf("FAIL %s: surfaceless context\n", who);
		return -1;
	}
	createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	imageTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	printf("%s: GL_RENDERER %s, dma_buf_import %s\n", who, (const char *)glGetString(GL_RENDERER),
	       strstr(eglQueryString(x->dpy, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import") ? "yes" : "no");
	return 0;
}

/* Attach image to a texture + FBO; returns the FBO. */
static GLuint
fbo_for(EGLImageKHR img)
{
	GLuint tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	imageTargetTexture(GL_TEXTURE_2D, img);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	return fbo;
}

static unsigned
readpix(void)
{
	unsigned char p[4];
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	return p[0] << 16 | p[1] << 8 | p[2];
}

static void
sendfd(int s, int fd, unsigned stride)
{
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { (void *)&stride, sizeof(stride) };
	struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(int));
	sendmsg(s, &m, 0);
}

static int
recvfd(int s, unsigned *stride)
{
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { (void *)stride, sizeof(*stride) };
	struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
	if (recvmsg(s, &m, 0) <= 0)
		return -1;
	int fd;
	memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof(int));
	return fd;
}

int
main(int argc, char **argv)
{
	int sv[2];
	if (getenv("DRM_RENDER") != NULL && *getenv("DRM_RENDER") != '\0')
		node = getenv("DRM_RENDER");
	if (argc > 1)
		node = argv[1];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	pid_t pid = fork();
	if (pid == 0) {
		/* importer */
		struct ctx x;
		unsigned stride;
		char ok;
		close(sv[0]);
		int fd = recvfd(sv[1], &stride);
		if (setup(&x, "importer") < 0)
			_exit(1);
		CHECK(fd >= 0, "importer received a dma-buf fd (stride %u)", stride);
		if (getenv("IMPORTER_DRAW_FIRST")) {
			/* draw into a private buffer first (submits to the host context) */
			struct gbm_bo *own = gbm_bo_create(x.gbm, W, H, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
			EGLImageKHR oi = createImage(x.dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, own, NULL);
			fbo_for(oi);
			glClearColor(0, 1, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			glFinish();
			CHECK(readpix() == 0x00ff00, "importer drew into its own buffer before importing");
		}
		EGLint attrs[] = {
			EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
			EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE };
		/* SKIP_A=1: import the fd only once, through (b) */
		EGLImageKHR a = getenv("SKIP_A") ? EGL_NO_IMAGE_KHR :
		    createImage(x.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
		if (!getenv("SKIP_A"))
			CHECK(a != EGL_NO_IMAGE_KHR, "(a) eglCreateImage(EGL_LINUX_DMA_BUF_EXT) as the first GL work (0x%x)", eglGetError());
		if (a != EGL_NO_IMAGE_KHR) {
			fbo_for(a);
			unsigned v = readpix();
			CHECK(v == 0x4080ff, "(a) imported buffer reads back the exporter's colour (0x%06x)", v);
		}
		struct gbm_import_fd_data d = { fd, W, H, stride, GBM_FORMAT_ARGB8888 };
		struct gbm_bo *bo = gbm_bo_import(x.gbm, GBM_BO_IMPORT_FD, &d, GBM_BO_USE_RENDERING);
		CHECK(bo != NULL, "(b) gbm_bo_import(GBM_BO_IMPORT_FD) (the glamor path)");
		if (bo) {
			EGLImageKHR b = createImage(x.dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
			CHECK(b != EGL_NO_IMAGE_KHR, "(b) eglCreateImage(EGL_NATIVE_PIXMAP_KHR) on the imported bo");
			if (b != EGL_NO_IMAGE_KHR) {
				fbo_for(b);
				unsigned v = readpix();
				CHECK(v == 0x4080ff, "(b) imported bo reads back the exporter's colour (0x%06x)", v);
				glClearColor(1, 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT);
				glFinish();
				v = readpix();
				CHECK(v == 0xff0000, "(b) importer drew red into the shared buffer (0x%06x)", v);
			}
		}
		write(sv[1], &ok, 1);
		read(sv[1], &ok, 1);
		printf("importer: %d failed\n", failures);
		_exit(failures != 0);
	}
	/* exporter */
	struct ctx x;
	close(sv[1]);
	if (setup(&x, "exporter") < 0)
		return 1;
	struct gbm_bo *bo = gbm_bo_create(x.gbm, W, H, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
	CHECK(bo != NULL, "exporter gbm_bo_create %dx%d ARGB8888", W, H);
	EGLImageKHR img = createImage(x.dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
	CHECK(img != EGL_NO_IMAGE_KHR, "exporter EGLImage on its bo");
	fbo_for(img);
	glClearColor(64 / 255.0, 128 / 255.0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	unsigned v = readpix();
	CHECK(v == 0x4080ff, "exporter cleared its buffer (0x%06x)", v);
	int fd = gbm_bo_get_fd(bo);
	CHECK(fd >= 0, "exporter gbm_bo_get_fd (PRIME export)");
	sendfd(sv[0], fd, gbm_bo_get_stride(bo));
	char ok;
	read(sv[0], &ok, 1);
	v = readpix();
	CHECK(v == 0xff0000, "exporter sees the importer's red in its own buffer (0x%06x)", v);
	write(sv[0], &ok, 1);
	int st;
	waitpid(pid, &st, 0);
	int child_failed = !WIFEXITED(st) || WEXITSTATUS(st) != 0;
	printf("dmabuf_import_test: exporter %d failed, importer %s\n", failures, child_failed ? "FAILED" : "ok");
	return failures != 0 || child_failed;
}
