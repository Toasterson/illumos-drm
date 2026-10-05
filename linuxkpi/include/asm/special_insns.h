/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's x86 cache-line flush instructions: clflush, and clflushopt
 * (66 0F AE /7) where the CPU has it, else clflush.
 */
#ifndef _ASM_SPECIAL_INSNS_H
#define	_ASM_SPECIAL_INSNS_H

#include <asm/cpufeature.h>

static inline void
clflush(volatile void *p)
{
	__asm__ __volatile__("clflush %0" : "+m" (*(volatile char *)p));
}

static inline void
clflushopt(volatile void *p)
{
	if (boot_cpu_has(X86_FEATURE_CLFLUSHOPT))
		__asm__ __volatile__(".byte 0x66; clflush %0"
		    : "+m" (*(volatile char *)p));
	else
		clflush(p);
}

static inline void
wbinvd(void)
{
	__asm__ __volatile__("wbinvd" ::: "memory");
}

#endif
