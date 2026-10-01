/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * A misc module that only runs drm_facade_check() in _init(9E): built by
 * run-r5a-test.sh against a copy of include/drm with another facade major
 * version or another size of struct drm_device, it must refuse to load.
 */

#include <drm/drm_facade.h>
#include <sys/modctl.h>

static struct modlmisc facade_mismatch_modlmisc = {
	&mod_miscops, "illumos-drm facade mismatch test"
};

static struct modlinkage facade_mismatch_modlinkage = {
	MODREV_1, { &facade_mismatch_modlmisc, NULL }
};

int
_init(void)
{
	int r;

	if ((r = drm_facade_check("facade_mismatch")) != 0)
		return (r);
	return (mod_install(&facade_mismatch_modlinkage));
}

int
_fini(void)
{
	return (mod_remove(&facade_mismatch_modlinkage));
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&facade_mismatch_modlinkage, mi));
}
