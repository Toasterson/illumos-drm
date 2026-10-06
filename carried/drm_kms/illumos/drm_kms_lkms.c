/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The facade 2.0 hooks of a Linux KMS device (struct
 * drm_illumos_lkms_funcs, drm_lkms.h): what Linux's DRM core does for a
 * DRIVER_MODESET device in drm_ioctl(), drm_file_alloc()/drm_file_free(),
 * drm_lastclose() and drm_dev_register()/drm_dev_unregister(), done here
 * for the carried KMS code on behalf of the Rust core. A driver points
 * drm_driver.illumos_lkms at drm_kms_illumos_funcs.
 *
 * ioctl
 *	Linux's mode-setting rows of drm_ioctls[] (drivers/gpu/drm/
 *	drm_ioctl.c of Linux v7.2), by command number, after the core's
 *	permission check and argument copy. ATTACHMODE and DETACHMODE are
 *	no-ops (drm_noop() in Linux). MODESET_CTL has no row in Linux v7.2
 *	(-EINVAL, as Linux's drm_ioctl() for a row without a function).
 *	Leases are not offered (CREATE/LIST/GET/REVOKE_LEASE: -ENOTTY), and
 *	the dumb-buffer commands are the core's (-ENOTTY should one arrive);
 *	every other number is -ENOTTY. A handler's Linux-only error number
 *	(-ERESTARTSYS, ...) is mapped to an illumos one (lkpi_errno()).
 * file_open
 *	initialises the file's KMS area (framebuffer and blob lists, the
 *	framebuffer lock, the pending events), as drm_file_alloc().
 * file_close
 *	drm_file_free()'s KMS part, in its order: the pending events are
 *	unlinked (sent later only to their completions and fences), then the
 *	file's framebuffers are removed (drm_fb_release(): planes using them
 *	are disabled) and its property blobs destroyed.
 * lastclose
 *	nothing: there is no fbdev emulation whose mode could be restored.
 * register_dev / unregister_dev
 *	drm_modeset_register_all() / drm_modeset_unregister_all(): the
 *	connectors (late_register hooks, e.g. i915's panel and backlight),
 *	planes, CRTCs and encoders.
 * mode_config
 *	what Linux's GET_CAP reports from dev->mode_config: the cursor size
 *	(64 when the driver set none), the preferred depth, the shadow
 *	preference, whether framebuffer modifiers are supported, and
 *	asynchronous page flips.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/export.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_lkms.h>
#include <uapi/drm/drm.h>

#include "drm_internal.h"
#include "drm_crtc_internal.h"
#include "drm_kms_illumos.h"

typedef int drm_kms_ioctl_fn(struct drm_device *dev, void *data,
    struct drm_file *file_priv);

static int
drm_kms_noop(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return (0);
}

#define	DRM_KMS_IOCTL(cmd, fn)	[_IOC_NR(cmd)] = (fn)

/* Linux v7.2's drm_ioctls[] rows of mode setting, by command number. */
static drm_kms_ioctl_fn *const drm_kms_ioctls[256] = {
	DRM_KMS_IOCTL(DRM_IOCTL_WAIT_VBLANK, drm_wait_vblank_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETRESOURCES, drm_mode_getresources),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETCRTC, drm_mode_getcrtc),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_SETCRTC, drm_mode_setcrtc),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETPLANE, drm_mode_getplane),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_SETPLANE, drm_mode_setplane),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_CURSOR, drm_mode_cursor_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETGAMMA, drm_mode_gamma_get_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_SETGAMMA, drm_mode_gamma_set_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETENCODER, drm_mode_getencoder),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETCONNECTOR, drm_mode_getconnector),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_ATTACHMODE, drm_kms_noop),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_DETACHMODE, drm_kms_noop),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETPROPERTY, drm_mode_getproperty_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_SETPROPERTY,
	    drm_connector_property_set_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETPROPBLOB, drm_mode_getblob_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETFB, drm_mode_getfb),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_GETFB2, drm_mode_getfb2_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_ADDFB, drm_mode_addfb_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_ADDFB2, drm_mode_addfb2_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_RMFB, drm_mode_rmfb_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_CLOSEFB, drm_mode_closefb_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_PAGE_FLIP, drm_mode_page_flip_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_DIRTYFB, drm_mode_dirtyfb_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
	    drm_mode_obj_get_properties_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_OBJ_SETPROPERTY,
	    drm_mode_obj_set_property_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_CURSOR2, drm_mode_cursor2_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_ATOMIC, drm_mode_atomic_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_CREATEPROPBLOB, drm_mode_createblob_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_MODE_DESTROYPROPBLOB,
	    drm_mode_destroyblob_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_CRTC_GET_SEQUENCE, drm_crtc_get_sequence_ioctl),
	DRM_KMS_IOCTL(DRM_IOCTL_CRTC_QUEUE_SEQUENCE,
	    drm_crtc_queue_sequence_ioctl),
};

static int
drm_kms_ioctl(struct drm_device *dev, unsigned int nr, void *data,
    struct drm_file *file_priv)
{
	drm_kms_ioctl_fn *fn;
	int ret;

	if (nr == _IOC_NR(DRM_IOCTL_MODESET_CTL))
		return (-EINVAL);
	if (nr >= ARRAY_SIZE(drm_kms_ioctls) ||
	    (fn = drm_kms_ioctls[nr]) == NULL)
		return (-ENOTTY);
	/*
	 * The handlers return Linux's kernel-internal numbers too
	 * (-ERESTARTSYS from an interrupted lock or wait, ...), which Linux's
	 * syscall exit turns into a restart or EINTR: here they become the
	 * illumos errno user space sees, as for the driver's own ioctls
	 * (lkpi_ioctl_trampoline()).
	 */
	ret = fn(dev, data, file_priv);
	return (ret < 0 ? -lkpi_errno(-ret) : ret);
}

static int
drm_kms_file_open(struct drm_device *dev, struct drm_file *file)
{
	INIT_LIST_HEAD(&file->fbs);
	mutex_init(&file->fbs_lock);
	INIT_LIST_HEAD(&file->blobs);
	INIT_LIST_HEAD(&file->pending_event_list);
	return (0);
}

static void
drm_kms_file_close(struct drm_device *dev, struct drm_file *file)
{
	drm_kms_events_release(dev, file);
	drm_fb_release(file);
	drm_property_destroy_user_blobs(dev, file);
	mutex_destroy(&file->fbs_lock);
}

static void
drm_kms_lastclose(struct drm_device *dev)
{
}

static int
drm_kms_register_dev(struct drm_device *dev)
{
	int ret;

	/* Linux's drm_dev_register(): registered, then the objects. */
	WRITE_ONCE(dev->illumos_kms_registered, true);
	if ((ret = drm_modeset_register_all(dev)) != 0)
		WRITE_ONCE(dev->illumos_kms_registered, false);
	return (ret);
}

static void
drm_kms_unregister_dev(struct drm_device *dev)
{
	/* Linux's drm_dev_unregister(): unregistered, then the objects. */
	WRITE_ONCE(dev->illumos_kms_registered, false);
	drm_modeset_unregister_all(dev);
}

static void
drm_kms_mode_config(struct drm_device *dev,
    struct drm_illumos_mode_config *out)
{
	const struct drm_mode_config *mc = &dev->mode_config;

	out->cursor_width = mc->cursor_width != 0 ? mc->cursor_width : 64;
	out->cursor_height = mc->cursor_height != 0 ? mc->cursor_height : 64;
	out->preferred_depth = mc->preferred_depth;
	out->prefer_shadow = mc->prefer_shadow;
	out->fb_modifiers = !mc->fb_modifiers_not_supported;
	out->async_page_flip = mc->async_page_flip;
}

const struct drm_illumos_lkms_funcs drm_kms_illumos_funcs = {
	.ioctl = drm_kms_ioctl,
	.file_open = drm_kms_file_open,
	.file_close = drm_kms_file_close,
	.lastclose = drm_kms_lastclose,
	.register_dev = drm_kms_register_dev,
	.unregister_dev = drm_kms_unregister_dev,
	.mode_config = drm_kms_mode_config,
};
EXPORT_SYMBOL(drm_kms_illumos_funcs);
