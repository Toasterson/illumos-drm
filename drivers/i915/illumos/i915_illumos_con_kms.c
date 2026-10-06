/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The KMS side of the illumos console on i915n (display build): the
 * illumos counterpart of Linux's drm_fb_helper for the console that
 * gfx_private draws (i915_illumos_con.c). The console framebuffer is the
 * firmware's: the loader's framebuffer (fb_info), which the GOP left in
 * stolen memory and the GPU's aperture shows at fb_info.paddr, and which
 * i915 wraps at attach as the initial plane's framebuffer, pinned at the
 * same GGTT offset (intel_initial_plane_config()). gfx_private keeps
 * writing through the aperture at that address, so this file keeps that
 * framebuffer and its GGTT binding alive for the device's life (a
 * framebuffer reference and an extra VMA pin): whatever X does, the
 * aperture range keeps backing the console's memory.
 *
 * Restore (Linux: drm_fb_helper_restore_fbdev_mode_unlocked(), here
 * i915n_con_kms_restore()): an atomic commit of the configuration the
 * console had at attach (its CRTC with its mode, its connectors, the
 * console framebuffer on the CRTC's primary plane, every other plane and
 * CRTC off), from a work item so that the callers (gfx_private's
 * KD_TEXT hook under tem's locks, DRM lastclose) never commit themselves.
 * The work checks that the console still shows text with every modeset
 * lock held: X switches to KD_GRAPHICS before it sets its own mode, and
 * its commit waits for those locks, so a restore never overrides X.
 *
 * Panic (i915n_con_kms_panic()): register writes only, no locks, no
 * allocation: the console CRTC's primary plane scans out the console
 * framebuffer again. Not covered: a pipe X switched off, or drives with
 * another mode than the console's.
 */

#include <linux/pci.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_lkms.h>

#include "i915_drv.h"
#include "i915_vma.h"
#include "gt/intel_gt.h"
#include "gt/intel_gtt.h"
#include "display/intel_de.h"
#include "display/intel_display_core.h"
#include "display/intel_display_types.h"
#include "display/i9xx_plane_regs.h"

#include "i915_illumos_con.h"

/* misc/drm_kms */
extern const struct drm_illumos_lkms_funcs drm_kms_illumos_funcs;
extern struct pci_dev *lkpi_pci_dev_of(dev_info_t *dip);

#define	I915N_CON_MAX_CONNECTORS	8

static struct i915n_con_kms {
	bool			active;
	struct drm_device	*drm;
	struct drm_framebuffer	*fb;
	struct i915_vma		*vma;
	struct drm_crtc		*crtc;
	struct drm_connector	*conns[I915N_CON_MAX_CONNECTORS];
	unsigned int		nconns;
	struct drm_display_mode	mode;
	struct work_struct	restore_work;
} i915n_con;

/*
 * i915's hook table for misc/drm_kms (patch 0012): drm_kms's, with
 * lastclose also restoring the console, as Linux's lastclose restores
 * fbdev (drm_client_dev_restore()).
 */
struct drm_illumos_lkms_funcs i915n_lkms_funcs;

/*
 * The DRM master's file closed (X exited or died). A clean X exit has
 * switched the console to KD_TEXT already; X killed on its VT leaves the
 * VT layer in graphics mode with nothing on the screen (illumos' VT
 * driver, unlike Linux's, does not reset a VT whose controlling process
 * died). Take the display back for the console: the illumos side
 * switches gfx_private to KD_TEXT (which queues the restore) or, in
 * KD_TEXT already, queues it directly.
 */
static void
i915n_file_close(struct drm_device *dev, struct drm_file *file)
{
	bool master = READ_ONCE(file->is_master);

	drm_kms_illumos_funcs.file_close(dev, file);
	if (master && READ_ONCE(i915n_con.active) && i915n_con.drm == dev)
		i915n_con_master_gone();
}

static void
i915n_lastclose(struct drm_device *dev)
{
	if (drm_kms_illumos_funcs.lastclose != NULL)
		drm_kms_illumos_funcs.lastclose(dev);
	if (READ_ONCE(i915n_con.active) && i915n_con.drm == dev)
		i915n_con_kms_restore();
}

/* Does the console framebuffer match what gfx_private draws into? */
static bool
con_fb_matches(struct drm_device *drm, struct drm_framebuffer *fb,
    struct i915_vma *vma, u64 aperture, const struct i915n_con_fb *want)
{
	u64 addr = aperture + i915_ggtt_offset(vma);

	if (addr != want->paddr) {
		drm_dbg_kms(drm, "console: plane at 0x%llx, firmware "
		    "framebuffer at 0x%llx\n", addr, want->paddr);
		return (false);
	}
	if (fb->format->format != DRM_FORMAT_XRGB8888 &&
	    fb->format->format != DRM_FORMAT_ARGB8888) {
		drm_info(drm, "console: firmware framebuffer format %p4cc "
		    "is not XRGB8888\n", &fb->format->format);
		return (false);
	}
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    fb->pitches[0] != want->pitch || want->bpp != 4 ||
	    fb->width < want->width || fb->height < want->height ||
	    vma->size < (u64)want->pitch * want->height) {
		drm_info(drm, "console: firmware framebuffer %ux%u pitch %u "
		    "modifier 0x%llx does not cover the console's %ux%u "
		    "pitch %u\n", fb->width, fb->height, fb->pitches[0],
		    fb->modifier, want->width, want->height, want->pitch);
		return (false);
	}
	return (true);
}

int
i915n_con_kms_takeover(dev_info_t *dip, const struct i915n_con_fb *want)
{
	struct pci_dev *pdev = lkpi_pci_dev_of(dip);
	struct drm_device *drm = pdev != NULL ? pci_get_drvdata(pdev) : NULL;
	struct drm_i915_private *i915;
	struct drm_connector_list_iter iter;
	struct drm_connector *conn;
	struct drm_crtc *crtc, *found = NULL;
	struct drm_framebuffer *fb = NULL;
	struct i915_vma *vma = NULL;
	u64 aperture;

	if (drm == NULL || i915n_con.active)
		return (-EINVAL);
	i915 = to_i915(drm);
	if (i915->display == NULL || !HAS_DISPLAY(i915->display))
		return (-ENODEV);
	aperture = to_gt(i915)->ggtt->gmadr.start;

	drm_modeset_lock_all(drm);
	drm_for_each_crtc(crtc, drm) {
		struct drm_plane_state *ps = crtc->primary->state;

		if (crtc->state == NULL || !crtc->state->active ||
		    ps == NULL || ps->fb == NULL)
			continue;
		vma = to_intel_plane_state(ps)->ggtt_vma;
		if (vma != NULL &&
		    con_fb_matches(drm, ps->fb, vma, aperture, want)) {
			found = crtc;
			fb = ps->fb;
			break;
		}
	}
	if (found == NULL) {
		drm_modeset_unlock_all(drm);
		drm_info(drm, "console: no active plane shows the firmware "
		    "framebuffer at 0x%llx; no console on this device\n",
		    want->paddr);
		return (-ENOENT);
	}
	/* The console's memory and its GGTT binding, for the device's life. */
	drm_framebuffer_get(fb);
	__i915_vma_pin(vma);
	i915n_con.drm = drm;
	i915n_con.fb = fb;
	i915n_con.vma = vma;
	i915n_con.crtc = found;
	drm_mode_copy(&i915n_con.mode, &found->state->mode);
	i915n_con.nconns = 0;
	drm_connector_list_iter_begin(drm, &iter);
	drm_for_each_connector_iter(conn, &iter) {
		if (conn->state == NULL || conn->state->crtc != found ||
		    i915n_con.nconns == I915N_CON_MAX_CONNECTORS)
			continue;
		drm_connector_get(conn);
		i915n_con.conns[i915n_con.nconns++] = conn;
	}
	drm_connector_list_iter_end(&iter);
	drm_modeset_unlock_all(drm);
	WRITE_ONCE(i915n_con.active, true);
	drm_info(drm, "console: firmware framebuffer %ux%u (GGTT 0x%x) on "
	    "[CRTC:%d:%s], %u connector(s), mode " DRM_MODE_FMT "\n",
	    fb->width, fb->height, i915_ggtt_offset(vma), found->base.id,
	    found->name, i915n_con.nconns, DRM_MODE_ARG(&i915n_con.mode));
	return (0);
}

static bool
con_connector(struct drm_connector *conn)
{
	unsigned int i;

	for (i = 0; i < i915n_con.nconns; i++) {
		if (i915n_con.conns[i] == conn)
			return (true);
	}
	return (false);
}

/* Builds and commits the console configuration; -EDEADLK to back off. */
static int
con_commit(struct drm_atomic_commit *state)
{
	struct drm_device *drm = i915n_con.drm;
	struct drm_framebuffer *fb = i915n_con.fb;
	struct drm_connector_list_iter iter;
	struct drm_connector *conn;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	int ret;

	drm_for_each_crtc(crtc, drm) {
		struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state,
		    crtc);

		if (IS_ERR(cs))
			return (PTR_ERR(cs));
		if (crtc == i915n_con.crtc) {
			ret = drm_atomic_set_mode_for_crtc(cs, &i915n_con.mode);
			cs->active = true;
		} else {
			ret = drm_atomic_set_mode_for_crtc(cs, NULL);
			cs->active = false;
		}
		if (ret != 0)
			return (ret);
	}
	drm_for_each_plane(plane, drm) {
		struct drm_plane_state *ps = drm_atomic_get_plane_state(state,
		    plane);

		if (IS_ERR(ps))
			return (PTR_ERR(ps));
		if (plane == i915n_con.crtc->primary) {
			ret = drm_atomic_set_crtc_for_plane(ps, i915n_con.crtc);
			if (ret != 0)
				return (ret);
			drm_atomic_set_fb_for_plane(ps, fb);
			ps->crtc_x = 0;
			ps->crtc_y = 0;
			ps->crtc_w = i915n_con.mode.hdisplay;
			ps->crtc_h = i915n_con.mode.vdisplay;
			ps->src_x = 0;
			ps->src_y = 0;
			ps->src_w = (u32)i915n_con.mode.hdisplay << 16;
			ps->src_h = (u32)i915n_con.mode.vdisplay << 16;
			ps->rotation = DRM_MODE_ROTATE_0;
		} else {
			ret = drm_atomic_set_crtc_for_plane(ps, NULL);
			if (ret != 0)
				return (ret);
			drm_atomic_set_fb_for_plane(ps, NULL);
		}
	}
	ret = 0;
	drm_connector_list_iter_begin(drm, &iter);
	drm_for_each_connector_iter(conn, &iter) {
		struct drm_connector_state *cs =
		    drm_atomic_get_connector_state(state, conn);

		if (IS_ERR(cs)) {
			ret = PTR_ERR(cs);
			break;
		}
		ret = drm_atomic_set_crtc_for_connector(cs,
		    con_connector(conn) ? i915n_con.crtc : NULL);
		if (ret != 0)
			break;
	}
	drm_connector_list_iter_end(&iter);
	if (ret != 0)
		return (ret);
	return (drm_atomic_commit(state));
}

static void
con_restore_work(struct work_struct *work)
{
	struct drm_device *drm = i915n_con.drm;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	int ret;

	if (!READ_ONCE(i915n_con.active) || !i915n_con_text_mode())
		return;
	state = drm_atomic_commit_alloc(drm);
	if (state == NULL) {
		drm_err(drm, "console: no memory to restore the console\n");
		return;
	}
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
retry:
	ret = drm_modeset_lock_all_ctx(drm, &ctx);
	if (ret == 0) {
		/* With every modeset lock held: X's own commit waits. */
		if (!i915n_con_text_mode())
			ret = 0;
		else
			ret = con_commit(state);
	}
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	if (ret != 0)
		drm_err(drm, "console: restore failed (%d)\n", ret);
	drm_atomic_commit_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

void
i915n_con_kms_restore(void)
{
	if (READ_ONCE(i915n_con.active))
		(void) schedule_work(&i915n_con.restore_work);
}

void
i915n_con_kms_panic(void)
{
	struct intel_display *display;
	enum i9xx_plane_id p;
	u32 cntr;

	if (!READ_ONCE(i915n_con.active))
		return;
	display = to_i915(i915n_con.drm)->display;
	p = to_intel_plane(i915n_con.crtc->primary)->i9xx_plane;
	cntr = intel_de_read_fw(display, DSPCNTR(display, p));
	cntr &= ~(DISP_FORMAT_MASK | DISP_TILED);
	cntr |= DISP_ENABLE | DISP_FORMAT_BGRX888;
	intel_de_write_fw(display, DSPSTRIDE(display, p),
	    i915n_con.fb->pitches[0]);
	intel_de_write_fw(display, DSPLINOFF(display, p), 0);
	intel_de_write_fw(display, DSPTILEOFF(display, p), 0);
	intel_de_write_fw(display, DSPCNTR(display, p), cntr);
	/* The surface address last: it arms the update. */
	intel_de_write_fw(display, DSPSURF(display, p),
	    i915_ggtt_offset(i915n_con.vma));
}

void
i915n_con_kms_init(void)
{
	i915n_lkms_funcs = drm_kms_illumos_funcs;
	i915n_lkms_funcs.lastclose = i915n_lastclose;
	i915n_lkms_funcs.file_close = i915n_file_close;
	INIT_WORK(&i915n_con.restore_work, con_restore_work);
}

void
i915n_con_kms_fini(void)
{
	/*
	 * Reached only when no device is the console (a console device
	 * refuses detach, so the module stays loaded).
	 */
	(void) cancel_work_sync(&i915n_con.restore_work);
}
