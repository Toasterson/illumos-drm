/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * misc/drm_kms: Linux's DRM mode-setting code (the KMS object model, the
 * atomic core and its uAPI handlers, vblank, modes and EDID, the KMS,
 * DP, HDMI, DSC and SCDC helper libraries, HDMI infoframes), carried as
 * Linux C from the pinned tree (carried/drm_kms/tus.txt) for the Linux
 * display drivers on the illumos-drm stack (i915n). A driver points
 * drm_driver.illumos_lkms at drm_kms_illumos_funcs (drm_kms_lkms.c) and
 * the core hands the device's mode-setting ioctls and file events to this
 * module (facade 2.0, drm_lkms.h).
 *
 * _init sets up the one piece of global state that Linux's
 * drm_core_init() prepares for KMS, the connector name IDs; the rest of
 * the module's state belongs to the devices. It depends on misc/drm_core
 * (the facade; _init refuses a core whose facade differs from the one
 * this module was compiled against, as the drivers do), misc/linuxkpi
 * (the Linux kernel interfaces; _init checks its ABI) and
 * misc/drm_helpers.
 * The kernel keeps it loaded while a driver that depends on it
 * (-N misc/drm_kms) is loaded; drmadm's exclusion of misc/drm_core under
 * `select legacy` keeps it out of the legacy stack.
 */

#include <linux/module.h>
#include <lkpi/version.h>
#include <drm/drm_facade.h>

/* drivers/gpu/drm/drm_crtc_internal.h (drm_core_init() in Linux) */
extern void drm_connector_ida_init(void);
extern void drm_connector_ida_destroy(void);

static struct modlmisc drm_kms_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "Linux DRM KMS (v7.2)"
};

static struct modlinkage drm_kms_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &drm_kms_modlmisc, NULL }
};

int
_init(void)
{
	int ret;

	if ((ret = drm_facade_check("drm_kms")) != 0)
		return (ret);
	if ((ret = linuxkpi_check("drm_kms", LINUXKPI_ABI)) != 0)
		return (ret);
	/*
	 * Linux's drm_core_init(): the per-type connector name IDs
	 * (drm_connector_enum_list[].ida, "LVDS-1", "VGA-1", ...). Without
	 * them every drm_connector_init() fails.
	 */
	drm_connector_ida_init();
	if ((ret = mod_install(&drm_kms_modlinkage)) != 0)
		drm_connector_ida_destroy();
	return (ret);
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&drm_kms_modlinkage)) != 0)
		return (ret);
	/* Nothing of the layer may call into this module's text any more. */
	lkpi_module_quiesce(&drm_kms_modlinkage);
	drm_connector_ida_destroy();
	return (0);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&drm_kms_modlinkage, mi));
}
