/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The x86 processor facts the carried helpers read (asm/processor.h,
 * asm/cpufeature.h), from CPUID: whether CLFLUSH and CLFLUSHOPT exist and
 * the CLFLUSH line size. lkpi_x86_cpu_init() runs once, in the module's
 * _init before mod_install(9F); afterwards the values are only read, so
 * any number of threads may read them.
 *
 * To be reconciled with R5c: written for R5e before R5c's compatibility
 * layer existed; R5c's processor model replaces it.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>

struct cpuinfo_x86 boot_cpu_data;
uint64_t lkpi_x86_features;

static void
lkpi_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
	__asm__ __volatile__("cpuid"
	    : "=a" (r[0]), "=b" (r[1]), "=c" (r[2]), "=d" (r[3])
	    : "a" (leaf), "c" (sub));
}

int
lkpi_x86_cpu_init(void)
{
	uint32_t r[4];
	uint32_t max_leaf, line;
	uint64_t features = 0;

	lkpi_cpuid(0, 0, r);
	max_leaf = r[0];
	if (max_leaf < 1)
		return (ENOTSUP);
	lkpi_cpuid(1, 0, r);
	if ((r[3] & (1U << 19)) == 0)		/* CLFSH */
		return (ENOTSUP);
	features |= 1ULL << X86_FEATURE_CLFLUSH;
	line = ((r[1] >> 8) & 0xff) * 8;	/* CLFLUSH line size */
	if (line == 0 || (line & (line - 1)) != 0)
		return (ENOTSUP);
	if (max_leaf >= 7) {
		lkpi_cpuid(7, 0, r);
		if ((r[1] & (1U << 23)) != 0)	/* CLFLUSHOPT */
			features |= 1ULL << X86_FEATURE_CLFLUSHOPT;
	}
	boot_cpu_data.x86_clflush_size = (u16)line;
	lkpi_x86_features = features;
	return (0);
}
