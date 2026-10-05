/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The interface of Linux's intel-gtt module (drivers/char/agp/intel-gtt.c,
 * include/drm/intel/intel-gtt.h: GPL-2.0; the declarations are
 * reimplemented, decision D7). It serves the GTT of gen2-gen5 GPUs, which
 * the illumos i915 build does not support: drivers/i915/illumos provides
 * these functions as failures (intel_gmch_probe() returns 0, "no GMCH").
 */
#ifndef _DRM_INTEL_GTT_H
#define	_DRM_INTEL_GTT_H

#include <linux/types.h>

struct agp_bridge_data;
struct pci_dev;
struct sg_table;

extern void intel_gmch_gtt_get(u64 *gtt_total, phys_addr_t *mappable_base,
    resource_size_t *mappable_end);
extern int intel_gmch_probe(struct pci_dev *bridge_pdev,
    struct pci_dev *gpu_pdev, struct agp_bridge_data *bridge);
extern void intel_gmch_remove(void);
extern bool intel_gmch_enable_gtt(void);
extern void intel_gmch_gtt_flush(void);
extern void intel_gmch_gtt_insert_page(dma_addr_t addr, unsigned int pg,
    unsigned int flags);
extern void intel_gmch_gtt_insert_sg_entries(struct sg_table *st,
    unsigned int pg_start, unsigned int flags);
extern void intel_gmch_gtt_clear_range(unsigned int first_entry,
    unsigned int num_entries);
extern dma_addr_t intel_gmch_gtt_read_entry(unsigned int pg,
    bool *is_present, bool *is_local);

#define	AGP_DCACHE_MEMORY		1
#define	AGP_PHYS_MEMORY			2
#define	AGP_USER_CACHED_MEMORY_GFDT	(1 << 3)

#endif
