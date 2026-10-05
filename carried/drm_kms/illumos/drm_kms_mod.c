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
 * The module has no global state. It depends on misc/drm_core (the
 * facade; _init refuses a core whose facade differs from the one this
 * module was compiled against, as the drivers do), misc/linuxkpi (the
 * Linux kernel interfaces; _init checks its ABI) and misc/drm_helpers.
 * The kernel keeps it loaded while a driver that depends on it
 * (-N misc/drm_kms) is loaded; drmadm's exclusion of misc/drm_core under
 * `select legacy` keeps it out of the legacy stack.
 */

#include <linux/module.h>
#include <lkpi/version.h>
#include <drm/drm_facade.h>

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
	return (mod_install(&drm_kms_modlinkage));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&drm_kms_modlinkage)) != 0)
		return (ret);
	/* Nothing of the layer may call into this module's text any more. */
	lkpi_module_quiesce(&drm_kms_modlinkage);
	return (0);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&drm_kms_modlinkage, mi));
}
