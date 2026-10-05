/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The illumos replacement of Linux's drivers/platform/x86/intel_ips.h
 * (GPL-2.0), which i915's intel_rps.c includes for the one function of the
 * intel_ips driver it looks up with symbol_get() (no such driver exists on
 * illumos; the layer's symbol_get() finds nothing). fetch-and-patch.sh
 * copies it over the original.
 */
#ifndef _ILLUMOS_INTEL_IPS_H
#define	_ILLUMOS_INTEL_IPS_H

void ips_link_to_i915_driver(void);

#endif
