/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's x86 feature tests for the carried helpers (carried/):
 * static_cpu_has() and boot_cpu_has() on the features they ask about.
 *
 * X86_FEATURE_CLFLUSH is a constant true: lkpi_x86_cpu_init() refuses a
 * processor without CLFLUSH (every amd64 processor has it), so the module
 * cannot be loaded on one, and the compiler drops the paths Linux takes
 * without it (wbinvd_on_all_cpus(), see asm/smp.h). Other features are
 * the bits lkpi_x86_cpu_init() read from CPUID. The feature numbers are
 * this header's own, not Linux's word/bit encoding.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names follow Linux v7.2's arch/x86/include/asm/cpufeature.h
 * and cpufeatures.h (GPL-2.0). This file is a reimplementation, not a
 * copy (decision D7).
 */

#ifndef _LINUXKPI_ASM_CPUFEATURE_H
#define	_LINUXKPI_ASM_CPUFEATURE_H

#include <linux/types.h>

#define	X86_FEATURE_CLFLUSH	0	/* CPUID.1:EDX[19] */
#define	X86_FEATURE_CLFLUSHOPT	1	/* CPUID.(7,0):EBX[23] */

/* Bit n set: feature n present (filled by lkpi_x86_cpu_init()). */
extern uint64_t lkpi_x86_features;

static inline bool
lkpi_x86_has(unsigned int feature)
{
	if (feature == X86_FEATURE_CLFLUSH)
		return (true);
	return ((lkpi_x86_features & (1ULL << feature)) != 0);
}

#define	static_cpu_has(f)	lkpi_x86_has(f)
#define	boot_cpu_has(f)		lkpi_x86_has(f)

#endif	/* _LINUXKPI_ASM_CPUFEATURE_H */
