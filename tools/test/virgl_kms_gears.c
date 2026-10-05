/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Mesa's virgl driver rendering to the screen through virtio_gpu's KMS,
 * without X: a GBM scanout surface on the primary node, an EGL window
 * surface on it, and a spinning ring of coloured triangles drawn with the
 * host's renderer. Every frame is shown with a page flip, and the program
 * waits for the flip-complete event before drawing the next, so the rate
 * it reports is the rate at which frames reached the display. At the end
 * it holds the last frame for a while so that a host screendump can see it.
 *
 * Needs DRM master: stop any X server on the card first.
 *
 *   gcc -m64 -Wall -o virgl_kms_gears virgl_kms_gears.c -I/usr/include/libdrm \
 *       -L/usr/lib/amd64 -R/usr/lib/amd64 \
 *       -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -lEGL -lGL -lgbm -ldrm -lm
 *   virgl_kms_gears [frames [hold seconds [card]]]  (600, 20, /dev/dri/card0)
 */

#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int fails;

static void
check(int ok, const char *what)
{
	(void) printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	(void) fflush(stdout);
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

static void
flip_done(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *data)
{
	(void) fd;
	(void) seq;
	(void) sec;
	(void) usec;
	*(int *)data = 0;
}

static void
draw(int frame, int w, int h)
{
	glViewport(0, 0, w, h);
	glClearColor(0.05f, 0.05f, 0.15f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-(double)w / h, (double)w / h, -1, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatef(frame * 2.0f, 0, 0, 1);
	for (int i = 0; i < 12; i++) {
		float a = i * (float)M_PI / 6;
		glBegin(GL_TRIANGLES);
		glColor3f(0.5f + 0.5f * cosf(a), 0.5f + 0.5f * cosf(a + 2.1f), 0.5f + 0.5f * cosf(a + 4.2f));
		glVertex2f(0.2f * cosf(a), 0.2f * sinf(a));
		glVertex2f(0.8f * cosf(a - 0.2f), 0.8f * sinf(a - 0.2f));
		glVertex2f(0.8f * cosf(a + 0.2f), 0.8f * sinf(a + 0.2f));
		glEnd();
	}
}

static uint32_t
fb_for(int fd, struct gbm_bo *bo)
{
	uint32_t fb = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
	uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
	uint32_t pitches[4] = { gbm_bo_get_stride(bo) };
	uint32_t offsets[4] = { 0 };

	if (fb != 0)
		return (fb);
	if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo),
	    handles, pitches, offsets, &fb, 0) != 0) {
		perror("drmModeAddFB2");
		return (0);
	}
	gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb, NULL);
	return (fb);
}

int
main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 600;
	int hold = argc > 2 ? atoi(argv[2]) : 20;
	const char *card = argc > 3 ? argv[3] : "/dev/dri/card0";
	int fd = open(card, O_RDWR);
	drmModeRes *res;
	drmModeConnector *conn = NULL;
	drmModeModeInfo mode;
	uint32_t crtc = 0;
	struct gbm_device *gbm;
	struct gbm_surface *surf;
	EGLDisplay dpy;
	EGLConfig cfgs[64], cfg = NULL;
	EGLContext ctx;
	EGLSurface esurf;
	EGLint major, minor, n;
	static const EGLint cattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
	    EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	struct gbm_bo *bo, *prev = NULL;
	const char *renderer;

	if (fd < 0) {
		perror(card);
		return (1);
	}
	check(drmSetMaster(fd) == 0, "DRM master on the primary node");
	res = drmModeGetResources(fd);
	for (int i = 0; res != NULL && i < res->count_connectors && conn == NULL; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn != NULL && (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0)) {
			drmModeFreeConnector(conn);
			conn = NULL;
		}
	}
	check(conn != NULL, "a connected connector with modes");
	if (conn == NULL)
		return (1);
	mode = conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++) {
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = conn->modes[i];
			break;
		}
	}
	if (conn->encoder_id != 0) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
		if (enc != NULL) {
			crtc = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	if (crtc == 0 && conn->count_encoders > 0) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[0]);
		for (int i = 0; enc != NULL && i < res->count_crtcs; i++) {
			if (enc->possible_crtcs & (1u << i)) {
				crtc = res->crtcs[i];
				break;
			}
		}
		if (enc != NULL)
			drmModeFreeEncoder(enc);
	}
	(void) printf("connector %u, CRTC %u, mode %s %ux%u@%u\n", conn->connector_id, crtc, mode.name,
	    mode.hdisplay, mode.vdisplay, mode.vrefresh);
	check(crtc != 0, "a CRTC for the connector");

	gbm = gbm_create_device(fd);
	check(gbm != NULL, "gbm_create_device on the primary node");
	surf = gbm == NULL ? NULL : gbm_surface_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
	    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	check(surf != NULL, "a GBM scanout surface");
	if (surf == NULL)
		return (1);
	dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
	check(dpy != EGL_NO_DISPLAY && eglInitialize(dpy, &major, &minor), "eglInitialize on the GBM device");
	check(eglBindAPI(EGL_OPENGL_API), "eglBindAPI(EGL_OPENGL_API)");
	if (eglChooseConfig(dpy, cattr, cfgs, 64, &n)) {
		for (int i = 0; i < n; i++) {
			EGLint id;
			if (eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id) && id == GBM_FORMAT_XRGB8888) {
				cfg = cfgs[i];
				break;
			}
		}
	}
	check(cfg != NULL, "an XRGB8888 window config");
	if (cfg == NULL)
		return (1);
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
	esurf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)surf, NULL);
	check(ctx != EGL_NO_CONTEXT && esurf != EGL_NO_SURFACE && eglMakeCurrent(dpy, esurf, esurf, ctx),
	    "a GL context current on the window surface");
	if (esurf == EGL_NO_SURFACE)
		return (1);
	renderer = (const char *)glGetString(GL_RENDERER);
	(void) printf("GL_RENDERER: %s\nGL_VERSION: %s\n", renderer, glGetString(GL_VERSION));
	check(renderer != NULL && strstr(renderer, "virgl") != NULL, "the renderer is virgl");

	draw(0, mode.hdisplay, mode.vdisplay);
	check(eglSwapBuffers(dpy, esurf), "the first frame");
	bo = gbm_surface_lock_front_buffer(surf);
	check(bo != NULL && fb_for(fd, bo) != 0, "a framebuffer for the first frame");
	if (bo == NULL)
		return (1);
	check(drmModeSetCrtc(fd, crtc, fb_for(fd, bo), 0, 0, &conn->connector_id, 1, &mode) == 0,
	    "mode set with the first frame");
	prev = bo;

	drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };
	int flips = 0, bad = 0;
	double t0 = now();
	for (int f = 1; f <= frames; f++) {
		int pending = 1;
		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		draw(f, mode.hdisplay, mode.vdisplay);
		if (!eglSwapBuffers(dpy, esurf)) {
			bad++;
			break;
		}
		bo = gbm_surface_lock_front_buffer(surf);
		if (bo == NULL || drmModePageFlip(fd, crtc, fb_for(fd, bo), DRM_MODE_PAGE_FLIP_EVENT, &pending) != 0) {
			bad++;
			break;
		}
		while (pending) {
			if (poll(&pfd, 1, 2000) != 1) {
				bad++;
				break;
			}
			(void) drmHandleEvent(fd, &ev);
		}
		if (pending)
			break;
		flips++;
		gbm_surface_release_buffer(surf, prev);
		prev = bo;
	}
	double dt = now() - t0;
	(void) printf("%d page-flipped virgl frames in %.3f s: %.1f frames/s (mode %u Hz)\n", flips, dt,
	    flips / dt, mode.vrefresh);
	check(bad == 0 && flips == frames && glGetError() == GL_NO_ERROR, "every frame rendered and flipped");
	(void) printf("holding the last frame for %d s\n", hold);
	(void) fflush(stdout);
	(void) sleep(hold);

	(void) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	(void) eglDestroySurface(dpy, esurf);
	(void) eglDestroyContext(dpy, ctx);
	(void) eglTerminate(dpy);
	(void) printf("virgl_kms_gears: %d failed\n", fails);
	return (fails != 0);
}
