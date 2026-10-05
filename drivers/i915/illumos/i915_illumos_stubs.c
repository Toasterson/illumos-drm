/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The parts of i915 the illumos headless build leaves out, as functions
 * that answer for a device without them (R5c):
 *
 * - local memory and TTM (discrete GPUs only). i915 sets up a TTM device
 *   on every GPU (intel_region_ttm_device_init() from
 *   i915_driver_early_probe()) but integrated GPUs (gen7 Ivy Bridge,
 *   gen9.5 Coffee Lake) keep their objects in shmem and stolen memory and
 *   never use it. TTM is not carried (decision R5c: stub, not carry,
 *   ttm_device_init for integrated GPUs): the device init succeeds
 *   without a TTM device, the TTM region setup and local-memory probe fail
 *   with -ENODEV (reached only for IS_DGFX/HAS_LMEM parts, which this
 *   build does not support), and the TTM and buddy-allocator functions
 *   that compiled i915 code references only on those paths warn.
 * - the GSC (Graphics System Controller, discrete GPUs and Meteor Lake):
 *   no GSC, its interrupt does nothing.
 * - the gen2-gen5 GTT through intel-gtt: probing fails with -ENODEV.
 * - the KMS helpers i915 calls on its display paths (no display).
 */

#include "i915_drv.h"

#include <drm/drm_atomic_helper.h>
#include <drm/drm_buddy.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/ttm/ttm_resource.h>
#include <linux/gpu_buddy.h>
#include "intel_memory_region.h"
#include "intel_region_ttm.h"
#include "gem/i915_gem_ttm.h"
#include "gem/i915_gem_ttm_pm.h"
#include "gt/intel_ggtt_gmch.h"
#include "gt/intel_gsc.h"
#include "gt/intel_region_lmem.h"

#define	I915_ILLUMOS_UNSUPPORTED(what) \
	WARN_ONCE(1, "i915 (illumos): %s is not supported (local memory/TTM)", what)

/* --- TTM / local memory ---------------------------------------------- */

int
intel_region_ttm_device_init(struct drm_i915_private *dev_priv)
{
	/* No TTM device: integrated GPUs do not use one. */
	(void) dev_priv;
	return (0);
}

void
intel_region_ttm_device_fini(struct drm_i915_private *dev_priv)
{
	(void) dev_priv;
}

struct intel_memory_region *
i915_gem_ttm_system_setup(struct drm_i915_private *i915, u16 type,
    u16 instance)
{
	I915_ILLUMOS_UNSUPPORTED("the TTM system region");
	return (ERR_PTR(-ENODEV));
}

struct intel_memory_region *
intel_gt_setup_lmem(struct intel_gt *gt)
{
	(void) gt;
	return (ERR_PTR(-ENODEV));
}

int
i915_ttm_backup_region(struct intel_memory_region *mr, u32 flags)
{
	return (0);
}

void
i915_ttm_recover_region(struct intel_memory_region *mr)
{
}

int
i915_ttm_restore_region(struct intel_memory_region *mr, u32 flags)
{
	return (0);
}

bool
i915_ttm_resource_mappable(struct ttm_resource *res)
{
	I915_ILLUMOS_UNSUPPORTED("i915_ttm_resource_mappable");
	return (false);
}

void
ttm_resource_init(struct ttm_buffer_object *bo, const struct ttm_place *place,
    struct ttm_resource *res)
{
	I915_ILLUMOS_UNSUPPORTED("ttm_resource_init");
}

void
ttm_resource_fini(struct ttm_resource_manager *man, struct ttm_resource *res)
{
	I915_ILLUMOS_UNSUPPORTED("ttm_resource_fini");
}

void
ttm_resource_manager_init(struct ttm_resource_manager *man,
    struct ttm_device *bdev, uint64_t size)
{
	I915_ILLUMOS_UNSUPPORTED("ttm_resource_manager_init");
}

int
ttm_resource_manager_evict_all(struct ttm_device *bdev,
    struct ttm_resource_manager *man)
{
	I915_ILLUMOS_UNSUPPORTED("ttm_resource_manager_evict_all");
	return (-ENODEV);
}

void
ttm_resource_manager_debug(struct ttm_resource_manager *man,
    struct drm_printer *p)
{
	drm_printf(p, "  (no TTM on illumos)\n");
}

int
gpu_buddy_init(struct gpu_buddy *mm, u64 size, u64 chunk_size)
{
	I915_ILLUMOS_UNSUPPORTED("gpu_buddy_init");
	return (-ENODEV);
}

void
gpu_buddy_fini(struct gpu_buddy *mm)
{
}

int
gpu_buddy_alloc_blocks(struct gpu_buddy *mm, u64 start, u64 end, u64 size,
    u64 min_page_size, struct list_head *blocks, unsigned long flags)
{
	I915_ILLUMOS_UNSUPPORTED("gpu_buddy_alloc_blocks");
	return (-ENODEV);
}

void
gpu_buddy_free_list(struct gpu_buddy *mm, struct list_head *objects,
    unsigned int flags)
{
}

void
drm_buddy_print(struct gpu_buddy *mm, struct drm_printer *p)
{
	drm_printf(p, "  (no buddy allocator on illumos)\n");
}

void
drm_buddy_block_print(struct gpu_buddy *mm, struct gpu_buddy_block *block,
    struct drm_printer *p)
{
}

/* --- GSC ------------------------------------------------------------------ */

void
intel_gsc_init(struct intel_gsc *gsc, struct drm_i915_private *i915)
{
	(void) gsc;
	(void) i915;
}

void
intel_gsc_fini(struct intel_gsc *gsc)
{
	(void) gsc;
}

void
intel_gsc_irq_handler(struct intel_gt *gt, u32 iir)
{
	(void) gt;
	(void) iir;
}

/* --- gen2-gen5 GTT ------------------------------------------------------ */

int
intel_ggtt_gmch_probe(struct i915_ggtt *ggtt)
{
	drm_err(&ggtt->vm.i915->drm,
	    "gen2-gen5 GPUs are not supported by i915 on illumos\n");
	return (-ENODEV);
}

int
intel_ggtt_gmch_enable_hw(struct drm_i915_private *i915)
{
	return (-ENODEV);
}

void
intel_ggtt_gmch_flush(void)
{
}

/* --- KMS helpers on the display paths ------------------------------------- */

void
drm_atomic_helper_shutdown(struct drm_device *dev)
{
	(void) dev;
}

void
drm_kms_helper_poll_disable(struct drm_device *dev)
{
	(void) dev;
}

void
drm_kms_helper_poll_enable(struct drm_device *dev)
{
	(void) dev;
}

void
drm_mode_config_reset(struct drm_device *dev)
{
	(void) dev;
}
