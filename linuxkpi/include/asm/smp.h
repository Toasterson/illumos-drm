/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's asm/smp.h as the carried helpers (carried/) use it: drm_cache.c
 * includes it on x86 and relies on what it brings in (boot_cpu_data,
 * static_cpu_has(), clflushopt(), mb()), and names wbinvd_on_all_cpus()
 * for processors without CLFLUSH.
 *
 * wbinvd_on_all_cpus() has no definition on purpose: X86_FEATURE_CLFLUSH
 * is a constant true here (asm/cpufeature.h), so every call is dead code
 * the compiler removes, and the module's -z defs link fails if one ever
 * survives. A cross-call write-back of every cache is not provided.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names follow Linux v7.2's arch/x86/include/asm/smp.h
 * (GPL-2.0). This file is a reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_ASM_SMP_H
#define	_LINUXKPI_ASM_SMP_H

#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/special_insns.h>

extern void lkpi_wbinvd_on_all_cpus_is_not_provided(void);
#define	wbinvd_on_all_cpus()	lkpi_wbinvd_on_all_cpus_is_not_provided()

#endif	/* _LINUXKPI_ASM_SMP_H */
