/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's clflushopt() for the carried helpers (carried/): writes back
 * and invalidates the cache line holding the byte at `p`, with CLFLUSHOPT
 * where the processor has it (66-prefixed CLFLUSH, unordered: callers
 * fence with mb()) and CLFLUSH otherwise. Linux patches the instruction
 * in at boot; this tests the feature bit at each call.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names follow Linux v7.2's
 * arch/x86/include/asm/special_insns.h (GPL-2.0). This file is a
 * reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_ASM_SPECIAL_INSNS_H
#define	_LINUXKPI_ASM_SPECIAL_INSNS_H

#include <asm/cpufeature.h>

static inline void
clflush(volatile void *p)
{
	__asm__ __volatile__("clflush %0" : "+m" (*(volatile char *)p));
}

static inline void
clflushopt(volatile void *p)
{
	if (lkpi_x86_has(X86_FEATURE_CLFLUSHOPT)) {
		__asm__ __volatile__(".byte 0x66; clflush %0"
		    : "+m" (*(volatile char *)p));
	} else {
		clflush(p);
	}
}

#endif	/* _LINUXKPI_ASM_SPECIAL_INSNS_H */
