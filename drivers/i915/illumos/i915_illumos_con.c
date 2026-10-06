/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The illumos console on i915n's display (display build), the illumos
 * side; the KMS side is i915_illumos_con_kms.c. Design:
 * notes/i915n-kms-design.md section 4 in the workspace, summarised here.
 *
 * illumos fixes its console device at boot: consconfig() attaches the
 * PCI display node and opens its first minor node, and tem (the terminal
 * emulator behind the console and the virtual terminals) drives it
 * through VIS_DEVINIT, VIS_CONS* and KDSETMODE, with a polled vector for
 * panic. gfx_private (misc/gfx_private, the gate's contract-private
 * interface for DRM drivers) implements all of that for the loader's
 * framebuffer (fb_info), as vgatext uses it without a DRM driver and the
 * legacy i915 used it with one. i915n does what the legacy driver did:
 *
 *	- at attach, before the DRM device and its card/render minors
 *	  exist, gfxp_fb_attach() on i915n's own node and the minor node
 *	  "gfx<instance>" (DDI_NT_DISPLAY), so that it is the first minor:
 *	  the console and /dev/fb. Only on Ivy Bridge (the platform with
 *	  display, patch 0011) and only when the loader left a 32-bit RGB
 *	  framebuffer (UEFI GOP, or a VBE mode): VGA text mode is not
 *	  taken over;
 *	- after i915's probe, the KMS side keeps the firmware framebuffer
 *	  that gfx_private draws into (i915n_con_kms_takeover()); if it
 *	  cannot (no plane shows it), the console minor and gfx_private go
 *	  again before consconfig opens anything, and the device has no
 *	  console (consconfig then finds card0, which refuses VIS_DEVINIT,
 *	  and the system falls back to the serial console);
 *	- the console minor's open, close, ioctl, devmap and segmap go to
 *	  gfx_private; every other minor number to the DRM core;
 *	- gfx_private's mode hook (gfxp_bm_register_fbops(), setmode) records
 *	  KD_TEXT/KD_GRAPHICS; KD_TEXT (switch to a text VT, X exiting or
 *	  dying) queues the KMS side's restore of the console configuration;
 *	  the hook itself never commits (it runs under tem's and gfx_private's
 *	  locks);
 *	- a CB_CL_PANIC callback (panicstr set, other CPUs stopped, before
 *	  the panic banner) points the console CRTC's plane back at the
 *	  console framebuffer and switches gfx_private to KD_TEXT, so the
 *	  panic message appears even when X was in front;
 *	- a console device refuses detach (consconfig holds its node for the
 *	  system's life anyway).
 *
 * One console per system: the first Ivy Bridge device that takes it.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/stat.h>
#include <sys/kd.h>
#include <sys/gfx_private.h>
#include <drm/drm_drv.h>

#include "i915_illumos_con.h"

/*
 * The leading members of the gate's i86pc fb_info_t (uts/i86pc/sys/
 * framebuffer.h; that platform header is not installed), as far as this
 * file reads them. The loader's framebuffer description, filled before
 * the kernel starts and only read afterwards.
 */
struct i915n_boot_fb {
	int		fb_type;	/* fb_type_t */
	uint64_t	paddr;
	uint8_t		*fb;
	uint8_t		*shadow_fb;
	uint64_t	fb_size;
	uint32_t	pitch;
	uint8_t		bpp;		/* bytes per pixel */
	uint8_t		depth;		/* bits per pixel */
	uint8_t		fg_color;
	uint8_t		bg_color;
	struct { uint8_t pos, size; } red, green, blue;	/* rgb_info_t */
	struct { uint16_t x, y; } screen;
};
#define	I915N_FB_TYPE_RGB	3	/* FB_TYPE_RGB */

extern struct i915n_boot_fb fb_info;	/* unix */

/* sys/callb.h does not mix with the LinuxKPI prologue. */
typedef void *i915n_callb_id_t;
extern i915n_callb_id_t callb_add(boolean_t (*)(void *, int), void *, int,
    char *);
extern int callb_delete(i915n_callb_id_t);
#define	I915N_CB_CL_PANIC	5	/* CB_CL_PANIC */

/* The console minor: node minors 256 + instance, unused by the DRM core. */
#define	I915N_CON_MINOR_BASE	256
#define	I915N_CON_MINOR_MAX	64

static struct {
	dev_info_t		*dip;		/* the node with the console minor */
	gfxp_fb_softc_ptr_t	gfxp;
	boolean_t		console;	/* taken over: tem may use it */
	i915n_callb_id_t	panic_cb;
	volatile int		kd_mode;	/* KD_TEXT, KD_GRAPHICS */
	char			name[16];
} i915n_con_st = { .kd_mode = KD_TEXT };

/* Ivy Bridge GPUs (Linux's INTEL_IVB_*_IDS): display on illumos. */
static const uint16_t i915n_con_ids[] = {
	0x0152, 0x0156, 0x015a, 0x0162, 0x0166, 0x016a
};

static boolean_t
con_minor(dev_t dev)
{
	minor_t m = getminor(dev);

	return (m >= I915N_CON_MINOR_BASE &&
	    m < I915N_CON_MINOR_BASE + I915N_CON_MINOR_MAX &&
	    i915n_con_st.dip != NULL &&
	    m - I915N_CON_MINOR_BASE == (minor_t)ddi_get_instance(
	    i915n_con_st.dip));
}

boolean_t
i915n_con_text_mode(void)
{
	return (i915n_con_st.kd_mode == KD_TEXT);
}

/* gfx_private's mode hook: called under its lock, from tem or wc. */
static int
i915n_con_setmode(int mode)
{
	i915n_con_st.kd_mode = mode;
	membar_producer();
	if (mode == KD_TEXT && panicstr == NULL)
		i915n_con_kms_restore();
	return (0);
}

static struct gfxp_blt_ops i915n_con_ops = {
	.blt = NULL,
	.copy = NULL,
	.clear = NULL,
	.setmode = i915n_con_setmode
};

static boolean_t
i915n_con_panic(void *arg, int code)
{
	int rval;

	_NOTE(ARGUNUSED(arg, code));
	if (!i915n_con_st.console || i915n_con_st.kd_mode == KD_TEXT)
		return (B_TRUE);
	i915n_con_kms_panic();
	/* panicstr is set: gfx_private's lock is bypassed. */
	(void) gfxp_fb_ioctl(makedevice(ddi_driver_major(i915n_con_st.dip),
	    I915N_CON_MINOR_BASE + ddi_get_instance(i915n_con_st.dip)),
	    KDSETMODE, KD_TEXT, FKIOCTL, kcred, &rval, i915n_con_st.gfxp);
	return (B_TRUE);
}

static boolean_t
con_wanted(dev_info_t *dip)
{
	int vendor, device, inst;
	uint_t i;

	if (i915n_con_st.dip != NULL)
		return (B_FALSE);
	inst = ddi_get_instance(dip);
	if (inst < 0 || inst >= I915N_CON_MINOR_MAX)
		return (B_FALSE);
	vendor = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "vendor-id", -1);
	device = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "device-id", -1);
	if (vendor != 0x8086)
		return (B_FALSE);
	for (i = 0; i < sizeof (i915n_con_ids) / sizeof (i915n_con_ids[0]);
	    i++) {
		if (device == i915n_con_ids[i])
			break;
	}
	if (i == sizeof (i915n_con_ids) / sizeof (i915n_con_ids[0]))
		return (B_FALSE);
	/* A 32-bit RGB framebuffer from the loader (not VGA text). */
	if (fb_info.fb_type != I915N_FB_TYPE_RGB || fb_info.paddr == 0 ||
	    fb_info.bpp != 4 || fb_info.red.pos != 16 ||
	    fb_info.green.pos != 8 || fb_info.blue.pos != 0) {
		(dev_err)(dip, CE_NOTE, "!console: the loader's framebuffer "
		    "(type %d, %u bytes per pixel) is not one i915n can keep; "
		    "no console on this device", fb_info.fb_type, fb_info.bpp);
		return (B_FALSE);
	}
	return (B_TRUE);
}

int
i915n_con_attach_early(dev_info_t *dip)
{
	gfxp_fb_softc_ptr_t gfxp;
	int inst = ddi_get_instance(dip);

	if (!con_wanted(dip))
		return (DDI_SUCCESS);
	gfxp = gfxp_fb_softc_alloc();
	if (gfxp_fb_attach(dip, DDI_ATTACH, gfxp) != DDI_SUCCESS) {
		gfxp_fb_softc_free(gfxp);
		(dev_err)(dip, CE_WARN, "console: gfx_private refused the "
		    "framebuffer; no console on this device");
		return (DDI_SUCCESS);
	}
	(void) snprintf(i915n_con_st.name, sizeof (i915n_con_st.name),
	    "gfx%d", inst);
	if (ddi_create_minor_node(dip, i915n_con_st.name, S_IFCHR,
	    I915N_CON_MINOR_BASE + inst, DDI_NT_DISPLAY, 0) != DDI_SUCCESS) {
		(void) gfxp_fb_detach(dip, DDI_DETACH, gfxp);
		gfxp_fb_softc_free(gfxp);
		(dev_err)(dip, CE_WARN, "console: cannot create minor node %s",
		    i915n_con_st.name);
		return (DDI_SUCCESS);
	}
	gfxp_bm_register_fbops(gfxp, &i915n_con_ops);
	i915n_con_st.gfxp = gfxp;
	i915n_con_st.dip = dip;
	return (DDI_SUCCESS);
}

void
i915n_con_attach_undo(dev_info_t *dip)
{
	if (i915n_con_st.dip != dip || i915n_con_st.console)
		return;
	ddi_remove_minor_node(dip, i915n_con_st.name);
	(void) gfxp_fb_detach(dip, DDI_DETACH, i915n_con_st.gfxp);
	gfxp_fb_softc_free(i915n_con_st.gfxp);
	i915n_con_st.gfxp = NULL;
	i915n_con_st.dip = NULL;
}

int
i915n_con_attach_late(dev_info_t *dip)
{
	struct i915n_con_fb fb;

	if (i915n_con_st.dip != dip)
		return (DDI_SUCCESS);
	fb.paddr = fb_info.paddr;
	fb.size = fb_info.fb_size;
	fb.pitch = fb_info.pitch;
	fb.width = fb_info.screen.x;
	fb.height = fb_info.screen.y;
	fb.bpp = fb_info.bpp;
	if (i915n_con_kms_takeover(dip, &fb) != 0) {
		/* Nothing has opened the minor yet: consconfig waits for us. */
		i915n_con_attach_undo(dip);
		(dev_err)(dip, CE_WARN, "console: the firmware framebuffer could "
		    "not be kept; no console on this device");
		return (DDI_SUCCESS);
	}
	i915n_con_st.console = B_TRUE;
	i915n_con_st.panic_cb = callb_add(i915n_con_panic, NULL,
	    I915N_CB_CL_PANIC, "i915n console");
	return (DDI_SUCCESS);
}

boolean_t
i915n_con_is_console(dev_info_t *dip)
{
	return (i915n_con_st.dip == dip && i915n_con_st.console);
}

/* --- cb_ops: the console minor to gfx_private, the rest to the core --- */

int
i915n_con_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
	if (con_minor(*devp))
		return (gfxp_fb_open(devp, flag, otyp, credp,
		    i915n_con_st.gfxp));
	return (drm_open(devp, flag, otyp, credp));
}

int
i915n_con_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
	if (con_minor(dev))
		return (gfxp_fb_close(dev, flag, otyp, credp,
		    i915n_con_st.gfxp));
	return (drm_close(dev, flag, otyp, credp));
}

int
i915n_con_read(dev_t dev, struct uio *uiop, cred_t *credp)
{
	if (con_minor(dev))
		return (ENXIO);
	return (drm_read(dev, uiop, credp));
}

int
i915n_con_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	if (con_minor(dev)) {
		int ret;

		/* tem uses VIS_CONSCLEAR's *rvalp, which gfx_private leaves. */
		*rvalp = 0;
		ret = gfxp_fb_ioctl(dev, cmd, arg, mode, credp, rvalp,
		    i915n_con_st.gfxp);
		return (ret);
	}
	return (drm_ioctl(dev, cmd, arg, mode, credp, rvalp));
}

int
i915n_con_chpoll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp)
{
	if (con_minor(dev))
		return (ENXIO);
	return (drm_chpoll(dev, events, anyyet, reventsp, phpp));
}

int
i915n_con_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off, size_t len,
    size_t *maplen, uint_t model)
{
	if (con_minor(dev))
		return (gfxp_fb_devmap(dev, dhp, off, len, maplen, model,
		    i915n_con_st.gfxp));
	return (drm_devmap(dev, dhp, off, len, maplen, model));
}

int
i915n_con_segmap(dev_t dev, off_t off, struct as *asp, caddr_t *addrp,
    off_t len, uint_t prot, uint_t maxprot, uint_t flags, cred_t *credp)
{
	if (con_minor(dev))
		return (ddi_devmap_segmap(dev, off, asp, addrp, len, prot,
		    maxprot, flags, credp));
	return (drm_segmap(dev, off, asp, addrp, len, prot, maxprot, flags,
	    credp));
}

int
i915n_con_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg,
    void **resultp)
{
	dev_t dev = (dev_t)(uintptr_t)arg;

	if (!con_minor(dev))
		return (drm_getinfo(dip, cmd, arg, resultp));
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*resultp = i915n_con_st.dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*resultp = (void *)(uintptr_t)(getminor(dev) -
		    I915N_CON_MINOR_BASE);
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}
