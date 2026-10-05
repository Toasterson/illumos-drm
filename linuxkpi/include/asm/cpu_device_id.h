/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's x86 CPU identification (arch/x86/include/asm/cpu_device_id.h,
 * processor.h, cpufeature.h; GPL-2.0; reimplemented in src/lkpi_cpu.c
 * with the CPUID instruction). boot_cpu_data has the vendor, family,
 * model and stepping of the boot CPU; boot_cpu_has() tests the CPUID
 * feature bits the layer knows (CLFLUSH, PAT, SSE4.1, AVX, HYPERVISOR).
 */
#ifndef _ASM_CPU_DEVICE_ID_H
#define	_ASM_CPU_DEVICE_ID_H

#include <linux/types.h>
#include <linux/mod_devicetable.h>

#define	X86_VENDOR_INTEL	0
#define	X86_VENDOR_AMD		2
#define	X86_VENDOR_HYGON	9
#define	X86_VENDOR_UNKNOWN	0xff
#define	X86_VENDOR_ANY		0xfffe
#define	X86_FAMILY_ANY		0
#define	X86_MODEL_ANY		0
#define	X86_STEPPING_ANY	0
#define	X86_FEATURE_ANY		0
#define	X86_CPU_TYPE_ANY	0
#define	X86_CPU_ID_FLAG_ENTRY_VALID	1
#define	VFM_MAKE(v, f, m)	(((v) << 16) | ((f) << 8) | (m))
#define	VFM_MODEL(vfm)		((vfm) & 0xff)
#define	VFM_FAMILY(vfm)		(((vfm) >> 8) & 0xff)
#define	VFM_VENDOR(vfm)		(((vfm) >> 16) & 0xffff)

struct cpuinfo_x86 {
	u8 x86;
	u8 x86_vendor;
	u8 x86_model;
	u8 x86_stepping;
	u32 x86_vfm;
	u16 x86_clflush_size;
	int x86_cache_alignment;
	u32 x86_capability_lkpi[4];
};

extern struct cpuinfo_x86 boot_cpu_data;

#define	X86_FEATURE_CLFLUSH	(0 * 32 + 19)
#define	X86_FEATURE_PAT		(0 * 32 + 16)
#define	X86_FEATURE_XMM2	(0 * 32 + 26)
#define	X86_FEATURE_XMM4_1	(1 * 32 + 19)
#define	X86_FEATURE_AVX		(1 * 32 + 28)
#define	X86_FEATURE_HYPERVISOR	(1 * 32 + 31)
#define	X86_FEATURE_CLFLUSHOPT	(2 * 32 + 23)

#define	boot_cpu_has(bit) \
	((boot_cpu_data.x86_capability_lkpi[(bit) / 32] >> ((bit) % 32)) & 1)
#define	static_cpu_has(bit)	boot_cpu_has(bit)
#define	cpu_feature_enabled(bit)	boot_cpu_has(bit)
#define	this_cpu_has(bit)	boot_cpu_has(bit)

#define	X86_MATCH_VFM(vfm, data) { \
	.vendor = VFM_VENDOR(vfm), .family = VFM_FAMILY(vfm), \
	.model = VFM_MODEL(vfm), .steppings = X86_STEPPING_ANY, \
	.feature = X86_FEATURE_ANY, .flags = X86_CPU_ID_FLAG_ENTRY_VALID, \
	.driver_data = (kernel_ulong_t)(data) }
#define	X86_MATCH_INTEL_FAM6_MODEL(model, data) \
	X86_MATCH_VFM(VFM_MAKE(X86_VENDOR_INTEL, 6, INTEL_FAM6_##model), data)

extern const struct x86_cpu_id *x86_match_cpu(const struct x86_cpu_id *match);

#endif
