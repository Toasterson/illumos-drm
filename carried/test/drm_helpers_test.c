/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * drm_helpers_test: a pseudo driver that runs the self-tests of the
 * carried Linux helpers against misc/drm_helpers when it attaches, in the
 * style of illumos-ddi's rselftest. It is linked with -N misc/drm_helpers
 * -z defs and only the module's exported functions
 * (carried/drm_helpers/drm_helpers.mapfile), so the tests exercise the
 * code i915 will bind to.
 *
 * Each test's result is logged (passed tests to the system log only,
 * failed checks and failed tests to the console as well, CE_WARN), and the
 * totals are published as the named kstat drm_helpers_test:<instance>:
 * selftest (tests_passed, tests_failed, checks_passed, checks_failed,
 * seed, scale, usecs). The attach succeeds even when a test fails: the
 * point is to report failures, not to hide them behind a missing device.
 *
 * Properties (drm_helpers_test.conf): seed (the random seed, default
 * 20261004), scale (iteration multiplier 1..100, default 1; anything
 * else is reported and replaced by 1) and inject_failure
 * (default 0; 1 adds a test "inject_failure" whose one check fails, so
 * that a run can show the failure path: one failed test and check in the
 * kstat, a CE_WARN, and the attach still succeeding). A TEST DRIVER: not
 * for production systems. tools/test/run-r5e-test.sh drives it.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/kstat.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "selftest.h"

extern dev_info_t *st_kernel_dip;

struct drm_helpers_test_stats {
	kstat_named_t tests_passed;
	kstat_named_t tests_failed;
	kstat_named_t checks_passed;
	kstat_named_t checks_failed;
	kstat_named_t seed;
	kstat_named_t scale;
	kstat_named_t usecs;
};

static kstat_t *drm_helpers_test_ksp;

/* A test that fails on purpose (property inject_failure=1). */
static void
drm_helpers_test_inject(struct st_ctx *ctx)
{
	ST_CHECK(ctx, ctx->scale == 0, "injected failure (inject_failure=1)");
}

static const struct st_test drm_helpers_test_injected[] = {
	{ "inject_failure", drm_helpers_test_inject, 0 },
};

/* Runs one suite; adds its results to the kstat. */
static void
drm_helpers_test_suite(const char *suite, const struct st_test *tests,
    unsigned int n, uint64_t seed, uint32_t scale,
    struct drm_helpers_test_stats *st)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct st_ctx ctx;

		if ((tests[i].flags & ST_HOST_ONLY) != 0)
			continue;
		bzero(&ctx, sizeof (ctx));
		ctx.test = tests[i].name;
		ctx.scale = scale;
		st_seed(&ctx, seed + i);
		tests[i].fn(&ctx);
		st->checks_passed.value.ui64 += ctx.checks_passed;
		st->checks_failed.value.ui64 += ctx.checks_failed;
		if (ctx.checks_failed == 0 && ctx.checks_passed > 0) {
			st->tests_passed.value.ui32++;
			cmn_err(CE_NOTE, "!drm_helpers_test: %s/%s: ok "
			    "(%llu checks)", suite, tests[i].name,
			    (unsigned long long)ctx.checks_passed);
		} else {
			st->tests_failed.value.ui32++;
			cmn_err(CE_WARN, "drm_helpers_test: %s/%s: FAILED "
			    "(%llu of %llu checks)", suite, tests[i].name,
			    (unsigned long long)ctx.checks_failed,
			    (unsigned long long)(ctx.checks_passed +
			    ctx.checks_failed));
		}
	}
}

static int
drm_helpers_test_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	struct drm_helpers_test_stats *st;
	hrtime_t start;
	uint64_t seed;
	uint32_t scale;
	int instance, inject;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);
	instance = ddi_get_instance(dip);
	if (instance != 0 || st_kernel_dip != NULL) {
		cmn_err(CE_WARN, "drm_helpers_test: only instance 0 attaches");
		return (DDI_FAILURE);
	}
	seed = (uint64_t)(uint32_t)ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "seed", 20261004);
	scale = (uint32_t)ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "scale", 1);
	if (scale == 0 || scale > 100) {
		cmn_err(CE_WARN, "drm_helpers_test: scale %u outside 1..100; "
		    "using 1", scale);
		scale = 1;
	}
	inject = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "inject_failure", 0);

	drm_helpers_test_ksp = kstat_create("drm_helpers_test", instance,
	    "selftest", "misc", KSTAT_TYPE_NAMED,
	    sizeof (struct drm_helpers_test_stats) / sizeof (kstat_named_t), 0);
	if (drm_helpers_test_ksp == NULL)
		return (DDI_FAILURE);
	st = drm_helpers_test_ksp->ks_data;
	kstat_named_init(&st->tests_passed, "tests_passed", KSTAT_DATA_UINT32);
	kstat_named_init(&st->tests_failed, "tests_failed", KSTAT_DATA_UINT32);
	kstat_named_init(&st->checks_passed, "checks_passed",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&st->checks_failed, "checks_failed",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&st->seed, "seed", KSTAT_DATA_UINT64);
	kstat_named_init(&st->scale, "scale", KSTAT_DATA_UINT32);
	kstat_named_init(&st->usecs, "usecs", KSTAT_DATA_UINT64);
	st->seed.value.ui64 = seed;
	st->scale.value.ui32 = scale;

	st_kernel_dip = dip;
	start = gethrtime();
	drm_helpers_test_suite("drm_mm", drm_mm_selftests, drm_mm_nselftests,
	    seed, scale, st);
	drm_helpers_test_suite("drm_cache", drm_cache_selftests,
	    drm_cache_nselftests, seed + 1000, scale, st);
	if (inject == 1) {
		drm_helpers_test_suite("selftest", drm_helpers_test_injected, 1,
		    seed, scale, st);
	}
	st->usecs.value.ui64 = (uint64_t)(gethrtime() - start) / 1000;

	cmn_err(CE_NOTE, "drm_helpers_test: %u tests passed, %u failed; "
	    "%llu checks passed, %llu failed (seed %llu, scale %u, %llu ms)",
	    st->tests_passed.value.ui32, st->tests_failed.value.ui32,
	    (unsigned long long)st->checks_passed.value.ui64,
	    (unsigned long long)st->checks_failed.value.ui64,
	    (unsigned long long)seed, scale,
	    (unsigned long long)st->usecs.value.ui64 / 1000);
	kstat_install(drm_helpers_test_ksp);
	ddi_report_dev(dip);
	return (DDI_SUCCESS);
}

static int
drm_helpers_test_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	if (drm_helpers_test_ksp != NULL) {
		kstat_delete(drm_helpers_test_ksp);
		drm_helpers_test_ksp = NULL;
	}
	if (st_kernel_dip == dip)
		st_kernel_dip = NULL;
	return (DDI_SUCCESS);
}

/* No device nodes: there is nothing to look up. */
static int
drm_helpers_test_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg,
    void **result)
{
	return (DDI_FAILURE);
}

static struct cb_ops drm_helpers_test_cb_ops = {
	.cb_open = nodev,
	.cb_close = nodev,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nodev,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_MP | D_NEW,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops drm_helpers_test_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = drm_helpers_test_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = drm_helpers_test_attach,
	.devo_detach = drm_helpers_test_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &drm_helpers_test_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

static struct modldrv drm_helpers_test_modldrv = {
	&mod_driverops,
	"self-tests of misc/drm_helpers (test driver)",
	&drm_helpers_test_dev_ops
};

static struct modlinkage drm_helpers_test_modlinkage = {
	MODREV_1,
	{ &drm_helpers_test_modldrv, NULL }
};

int
_init(void)
{
	return (mod_install(&drm_helpers_test_modlinkage));
}

int
_fini(void)
{
	return (mod_remove(&drm_helpers_test_modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&drm_helpers_test_modlinkage, modinfop));
}
