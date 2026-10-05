/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 The illumos-drm authors. The TSC frequency in kHz
 * (src/lkpi_cpu.c: measured against gethrtime(9F) when the layer loads;
 * illumos has no DDI interface for it).
 */
#ifndef _ASM_TSC_H
#define	_ASM_TSC_H
extern unsigned int lkpi_tsc_khz(void);
#define	tsc_khz		lkpi_tsc_khz()
static inline unsigned long long
rdtsc(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
	return (((unsigned long long)hi << 32) | lo);
}
#define	rdtsc_ordered()	rdtsc()
#endif
