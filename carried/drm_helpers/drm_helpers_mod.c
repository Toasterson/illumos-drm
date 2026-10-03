/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * misc/drm_helpers: the Linux DRM helper code that lives inside Linux's
 * drm.ko and that C drivers on the illumos-drm facade call, carried as
 * Linux C (carried/<helper>/linux/, built from carried/fetch-and-patch's
 * output). Today that is drm_mm, the range allocator i915 manages its GTT
 * address spaces and stolen memory with, and drm_cache's
 * drm_clflush_virt_range(), its CPU cache write-back; see
 * carried/drm_helpers/drm_helpers.mapfile for the exported functions and
 * the README's "R5e: carried helpers" for what else is and is not carried.
 *
 * The module's only state is the processor facts drm_cache reads
 * (linuxkpi/lib/x86_cpu.c), filled in _init before mod_install(9F) and
 * read-only afterwards; _init refuses a processor without CLFLUSH. The
 * kernel keeps the module loaded while a module that depends on it
 * (-N misc/drm_helpers) is loaded. It depends on nothing but the kernel;
 * it neither uses nor guards against either DRM core, which is the job of
 * misc/drm_core and gfx-drm's misc/drm (a driver that links this module
 * also links misc/drm_core). Legacy gfx-drm's misc/drm exports drm_mm
 * functions under the same names; krtld resolves each module's symbols in
 * its own dependencies first, so the two never mix.
 */

#include <sys/types.h>
#include <sys/modctl.h>
#include <sys/cmn_err.h>

#include <asm/processor.h>

static struct modlmisc drm_helpers_modlmisc = {
	&mod_miscops,
	"Linux DRM helpers: drm_mm, drm_cache (v7.2)"
};

static struct modlinkage drm_helpers_modlinkage = {
	MODREV_1,
	{ &drm_helpers_modlmisc, NULL }
};

int
_init(void)
{
	int err;

	if ((err = lkpi_x86_cpu_init()) != 0) {
		cmn_err(CE_WARN, "drm_helpers: the processor lacks CLFLUSH "
		    "(or reports no CLFLUSH line size); not loading");
		return (err);
	}
	return (mod_install(&drm_helpers_modlinkage));
}

int
_fini(void)
{
	return (mod_remove(&drm_helpers_modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&drm_helpers_modlinkage, modinfop));
}
