/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's x86 memory barriers for the carried helpers (carried/): mb(),
 * rmb() and wmb() as full, load and store fences.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's
 * arch/x86/include/asm/barrier.h (GPL-2.0). This file is a
 * reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_ASM_BARRIER_H
#define	_LINUXKPI_ASM_BARRIER_H

#define	mb()	__asm__ __volatile__("mfence" : : : "memory")
#define	rmb()	__asm__ __volatile__("lfence" : : : "memory")
#define	wmb()	__asm__ __volatile__("sfence" : : : "memory")

#endif	/* _LINUXKPI_ASM_BARRIER_H */
