/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The misc module "linuxkpi": _init(9E) initialises the layer's parts in
 * dependency order and installs the module; a failing part unwinds the
 * ones before it. _fini(9E) removes the module first (it fails while a
 * driver that depends on the layer is loaded) and only then tears the
 * parts down in reverse order; it refuses (EBUSY) while an interrupt
 * handler could not be removed (free_irq(), src/lkpi_irq.c), because the
 * handler's trampoline is in this module. linuxkpi_check() is the version
 * handshake of the drivers (<lkpi/version.h>).
 *
 * lkpi_module_quiesce() is called by a driver's _fini(9E) after its
 * module_exit: the layer's deferred work must not run driver code once
 * the driver's text is unloaded.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
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

	if (lkpi_irq_stuck() != 0) {
		cmn_err(CE_WARN, "linuxkpi: %u interrupt handlers could not be "
		    "removed; not unloading", lkpi_irq_stuck());
		return (EBUSY);
	}
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

/*
 * Quiesces the layer for a driver whose text is [text, text + size):
 * waits for queued RCU callbacks, stops the driver's kthreads, disarms
 * its timers, runs what is queued on the shared workqueues, cancels the
 * driver's work items still queued or armed anywhere and waits for those
 * running. Everything removed is reported: module_exit should have done
 * it. Thread context.
 */
void
lkpi_module_quiesce_range(const void *text, size_t size)
{
	uintptr_t lo = (uintptr_t)text, hi = lo + size;
	int n, i;

	if (servicing_interrupt() || text == NULL || hi < lo) {
		WARN_ONCE(1, "linuxkpi: lkpi_module_quiesce_range(%p, %lu) "
		    "refused", text, (unsigned long)size);
		return;
	}
	/*
	 * Until a round finds nothing: work that ran may arm timers or queue
	 * RCU callbacks, and an RCU callback may queue work (queue_rcu_work()).
	 */
	n = 0;
	for (i = 0; i < 10; i++) {
		int r;

		rcu_barrier();
		r = lkpi_kthread_quiesce(lo, hi);
		r += lkpi_timer_quiesce(lo, hi);
		lkpi_work_flush_system();
		r += lkpi_work_quiesce(lo, hi);
		r += lkpi_timer_quiesce(lo, hi);
		rcu_barrier();
		r += lkpi_work_quiesce(lo, hi);
		n += r;
		if (r == 0)
			break;
	}
	if (n != 0)
		cmn_err(CE_WARN, "linuxkpi: %d timers, work items or kthreads "
		    "of the unloading driver were still active", n);
}

/* lkpi_module_quiesce_range() for the text of the module of ml. */
void
lkpi_module_quiesce(struct modlinkage *ml)
{
	modctl_t *mc = ml != NULL ? mod_getctl(ml) : NULL;

	if (mc == NULL || mc->mod_text == NULL) {
		WARN_ONCE(1, "linuxkpi: lkpi_module_quiesce(): no module text");
		return;
	}
	lkpi_module_quiesce_range(mc->mod_text, mc->mod_text_size);
}
