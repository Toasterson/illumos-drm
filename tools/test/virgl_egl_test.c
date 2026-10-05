/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Mesa's virgl driver on virtio_gpu, without X: a GBM device on the render
 * node, an EGL display on it, a surfaceless desktop GL context. Prints the
 * GL vendor, renderer and version (the renderer names virgl when Mesa uses
 * the device), clears a 256x256 framebuffer object to a known colour and
 * reads it back, then times 600 clear-and-read-back frames.
 *
 * Build on illumos with Mesa's headers and libraries:
 *   gcc -m64 -Wall -o virgl_egl_test virgl_egl_test.c \
 *       -L/usr/lib/amd64 -R/usr/lib/amd64 -lEGL -lGL -lgbm
 * Usage: virgl_egl_test [render node] (default /dev/dri/renderD128).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <gbm.h>

#define	GL_FRAMEBUFFER			0x8D40
#define	GL_RENDERBUFFER			0x8D41
#define	GL_COLOR_ATTACHMENT0		0x8CE0
#define	GL_FRAMEBUFFER_COMPLETE		0x8CD5
#define	GL_RGBA8_			0x8058

typedef void (*gen_t)(GLsizei, GLuint *);
typedef void (*bind_t)(GLenum, GLuint);
typedef void (*storage_t)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*attach_t)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*status_t)(GLenum);

static int fails;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		fails++;
}

static double
now(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

int
main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd = open(node, O_RDWR);
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLint major, minor, n;
	EGLConfig cfg;
	EGLContext ctx;
	const char *ext, *renderer;
	static const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	GLuint fbo, rb;
	unsigned char px[4 * 256 * 256];

	if (fd < 0) {
		perror(node);
		return (1);
	}
	gbm = gbm_create_device(fd);
	check(gbm != NULL, "gbm_create_device on the render node");
	if (gbm == NULL)
		return (1);
	(void) printf("GBM backend: %s\n", gbm_device_get_backend_name(gbm));
	dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
	check(dpy != EGL_NO_DISPLAY && eglInitialize(dpy, &major, &minor), "eglInitialize on the GBM device");
	if (dpy == EGL_NO_DISPLAY)
		return (1);
	(void) printf("EGL %d.%d, vendor %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));
	ext = eglQueryString(dpy, EGL_EXTENSIONS);
	check(ext != NULL && strstr(ext, "EGL_KHR_surfaceless_context") != NULL, "EGL_KHR_surfaceless_context");
	check(eglBindAPI(EGL_OPENGL_API), "eglBindAPI(EGL_OPENGL_API)");
	check(eglChooseConfig(dpy, cattr, &cfg, 1, &n) && n == 1, "an EGL config for desktop GL");
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
	check(ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx),
	    "a surfaceless GL context is current");
	if (ctx == EGL_NO_CONTEXT)
		return (1);
	renderer = (const char *)glGetString(GL_RENDERER);
	(void) printf("GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_VENDOR),
	    renderer, glGetString(GL_VERSION));
	check(renderer != NULL && strstr(renderer, "virgl") != NULL, "the renderer is virgl (Mesa on virtio_gpu)");

	gen_t genfb = (gen_t)eglGetProcAddress("glGenFramebuffers");
	gen_t genrb = (gen_t)eglGetProcAddress("glGenRenderbuffers");
	bind_t bindfb = (bind_t)eglGetProcAddress("glBindFramebuffer");
	bind_t bindrb = (bind_t)eglGetProcAddress("glBindRenderbuffer");
	storage_t storage = (storage_t)eglGetProcAddress("glRenderbufferStorage");
	attach_t attach = (attach_t)eglGetProcAddress("glFramebufferRenderbuffer");
	status_t status = (status_t)eglGetProcAddress("glCheckFramebufferStatus");
	if (!genfb || !genrb || !bindfb || !bindrb || !storage || !attach || !status) {
		check(0, "framebuffer object entry points");
		return (1);
	}
	genfb(1, &fbo);
	genrb(1, &rb);
	bindrb(GL_RENDERBUFFER, rb);
	storage(GL_RENDERBUFFER, GL_RGBA8_, 256, 256);
	bindfb(GL_FRAMEBUFFER, fbo);
	attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	check(status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "a complete 256x256 RGBA8 framebuffer object");
	glViewport(0, 0, 256, 256);
	glClearColor(0.25f, 0.5f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int bad = 0;
	for (int i = 0; i < 256 * 256; i++) {
		unsigned char *p = px + 4 * i;
		if (abs(p[0] - 64) > 2 || abs(p[1] - 128) > 2 || p[2] < 253 || p[3] < 253)
			bad++;
	}
	(void) printf("first pixel %u %u %u %u, %d of 65536 differ\n", px[0], px[1], px[2], px[3], bad);
	check(bad == 0 && glGetError() == GL_NO_ERROR, "glClear on the host renderer reads back as (64, 128, 255, 255)");

	double t0 = now();
	for (int f = 0; f < 600; f++) {
		glClearColor((f % 256) / 255.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	double dt = now() - t0;
	(void) printf("600 clear-and-read-back frames in %.3f s: %.1f frames/s\n", dt, 600 / dt);
	check(glGetError() == GL_NO_ERROR, "600 frames without a GL error");

	(void) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	(void) eglDestroyContext(dpy, ctx);
	(void) eglTerminate(dpy);
	gbm_device_destroy(gbm);
	(void) close(fd);
	(void) printf("virgl_egl_test: %d failed\n", fails);
	return (fails != 0);
}
