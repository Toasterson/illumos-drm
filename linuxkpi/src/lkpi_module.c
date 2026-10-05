/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The misc module "linuxkpi": _init(9E) initialises the layer's parts in
 * dependency order and installs the module; a failing part unwinds the
 * ones before it. _fini(9E) removes the module first (it fails while a
 * driver that depends on the layer is loaded) and only then tears the
 * parts down in reverse order. linuxkpi_check() is the version handshake
 * of the drivers (<lkpi/version.h>).
 */

#include <linux/kernel.h>
#include <lkpi/version.h>
#include "lkpi_internal.h"

static struct modlmisc lkpi_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "LinuxKPI layer"
};

static struct modlinkage lkpi_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &lkpi_modlmisc, NULL }
};

/* The parts with state; lkpi_cpu has nothing to tear down. */
static const struct {
	const char *lp_name;
	int (*lp_init)(void);
	void (*lp_fini)(void);
} lkpi_parts[] = {
	{ "cpu", lkpi_cpu_init, NULL },
	{ "slab", lkpi_slab_init, lkpi_slab_fini },
	{ "page", lkpi_page_init, lkpi_page_fini },
	{ "sched", lkpi_sched_init, lkpi_sched_fini },
	{ "rcu", lkpi_rcu_init, lkpi_rcu_fini },
	{ "timer", lkpi_timer_init, lkpi_timer_fini },
	{ "work", lkpi_work_init, lkpi_work_fini },
	{ "drm", lkpi_drm_init, lkpi_drm_fini },
	{ "shrinker", lkpi_shrinker_init, lkpi_shrinker_fini },
	{ "pci", lkpi_pci_init, lkpi_pci_fini }
};

static void
lkpi_parts_fini(int n)
{
	while (n-- > 0) {
		if (lkpi_parts[n].lp_fini != NULL)
			lkpi_parts[n].lp_fini();
	}
}

int
_init(void)
{
	int i, ret;

	for (i = 0; i < (int)ARRAY_SIZE(lkpi_parts); i++) {
		ret = lkpi_parts[i].lp_init();
		if (ret != 0) {
			cmn_err(CE_WARN, "linuxkpi: %s initialisation failed "
			    "(%d)", lkpi_parts[i].lp_name, ret);
			lkpi_parts_fini(i);
			/*
			 * The parts return 0 or a negative Linux errno; the
			 * classic ones (up to ERANGE) are illumos' too.
			 */
			if (ret < 0)
				ret = (-ret <= ERANGE) ? -ret : EIO;
			return (ret);
		}
	}
	ret = mod_install(&lkpi_modlinkage);
	if (ret != 0)
		lkpi_parts_fini((int)ARRAY_SIZE(lkpi_parts));
	return (ret);
}

int
_fini(void)
{
	int ret;

	ret = mod_remove(&lkpi_modlinkage);
	if (ret != 0)
		return (ret);
	lkpi_parts_fini((int)ARRAY_SIZE(lkpi_parts));
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&lkpi_modlinkage, modinfop));
}

int
linuxkpi_check(const char *driver, unsigned int abi)
{
	if (abi == LINUXKPI_ABI)
		return (0);
	cmn_err(CE_WARN, "linuxkpi: driver %s was built for LinuxKPI ABI %u, "
	    "this module provides %u", driver != NULL ? driver : "?", abi,
	    LINUXKPI_ABI);
	return (EINVAL);
}
