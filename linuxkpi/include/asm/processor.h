/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's boot_cpu_data for the carried helpers (carried/), reduced to
 * the one field they read: the cache-line size CLFLUSH works on, from
 * CPUID leaf 1. lkpi_x86_cpu_init() (linuxkpi/lib/x86_cpu.c) fills it in
 * the module's _init before anything can call into the module.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names follow Linux v7.2's arch/x86/include/asm/processor.h
 * (GPL-2.0). This file is a reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_ASM_PROCESSOR_H
#define	_LINUXKPI_ASM_PROCESSOR_H

#include <linux/types.h>

struct cpuinfo_x86 {
	u16 x86_clflush_size;	/* bytes; a power of two */
};

extern struct cpuinfo_x86 boot_cpu_data;

/*
 * Reads the processor's features (linuxkpi/lib/x86_cpu.c): 0, or an
 * errno when the processor lacks what the helpers need (CLFLUSH).
 */
extern int lkpi_x86_cpu_init(void);

#endif	/* _LINUXKPI_ASM_PROCESSOR_H */
