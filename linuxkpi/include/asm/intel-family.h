/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Intel CPU family/model identifiers in Linux's "VFM" encoding (vendor,
 * family, model), the subset i915 compares against. The values are
 * Intel's CPUID models.
 */
#ifndef _ASM_INTEL_FAMILY_H
#define	_ASM_INTEL_FAMILY_H
#include <asm/cpu_device_id.h>
#define	IFM(f, m)	VFM_MAKE(X86_VENDOR_INTEL, f, m)
#define	INTEL_SANDYBRIDGE	IFM(6, 0x2A)
#define	INTEL_IVYBRIDGE		IFM(6, 0x3A)
#define	INTEL_HASWELL		IFM(6, 0x3C)
#define	INTEL_BROADWELL		IFM(6, 0x3D)
#define	INTEL_SKYLAKE_L		IFM(6, 0x4E)
#define	INTEL_SKYLAKE		IFM(6, 0x5E)
#define	INTEL_KABYLAKE_L	IFM(6, 0x8E)
#define	INTEL_KABYLAKE		IFM(6, 0x9E)
#define	INTEL_ROCKETLAKE	IFM(6, 0xA7)
#define	INTEL_COMETLAKE		IFM(6, 0xA5)
#define	INTEL_COMETLAKE_L	IFM(6, 0xA6)
#define	INTEL_ICELAKE_L		IFM(6, 0x7E)
#define	INTEL_TIGERLAKE_L	IFM(6, 0x8C)
#define	INTEL_TIGERLAKE		IFM(6, 0x8D)
#define	INTEL_ALDERLAKE		IFM(6, 0x97)
#define	INTEL_ALDERLAKE_L	IFM(6, 0x9A)
#define	INTEL_RAPTORLAKE	IFM(6, 0xB7)
#define	INTEL_RAPTORLAKE_P	IFM(6, 0xBA)
#define	INTEL_RAPTORLAKE_S	IFM(6, 0xBF)
#define	INTEL_METEORLAKE_L	IFM(6, 0xAA)
#define	INTEL_ATOM_SILVERMONT	IFM(6, 0x37)
#define	INTEL_ATOM_AIRMONT	IFM(6, 0x4C)
#define	INTEL_ATOM_GOLDMONT	IFM(6, 0x5C)
#define	INTEL_ATOM_GOLDMONT_PLUS	IFM(6, 0x7A)
#define	INTEL_ATOM_TREMONT	IFM(6, 0x86)
#define	INTEL_ATOM_TREMONT_L	IFM(6, 0x9C)
#define	INTEL_ATOM_GRACEMONT	IFM(6, 0xBE)
#endif
